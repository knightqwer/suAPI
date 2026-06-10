#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <regex>
#include <stdexcept>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <utility>
#include <curl/curl.h>
#include "httplib.h"
using namespace std;

static size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    static_cast<string*>(userp)->append(ptr, total);
    return total;
}

class Curl {
public:
    Curl() : handle_(curl_easy_init())
    {
        if (!handle_) throw runtime_error("curl init failed");
    }
    ~Curl() { curl_easy_cleanup(handle_); }
    Curl(const Curl&) = delete;
    Curl& operator=(const Curl&) = delete;
    CURL* get() const { return handle_; }
private:
    CURL* handle_;
};

static string urlEncode(CURL* curl, const string& raw)
{
    char* enc = curl_easy_escape(curl, raw.c_str(), (int)raw.size());
    if (!enc) throw runtime_error("url-encode failed");
    string out(enc);
    curl_free(enc);
    return out;
}

static string httpRequest(const string& url, const string& body)
{
    Curl curl;
    string response;
    struct curl_slist* headers = nullptr;
    if (!body.empty()) {
        headers = curl_slist_append(
            headers, "Content-Type: application/x-www-form-urlencoded");
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    if (!body.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        throw runtime_error(string("upstream request failed: ") +
                            curl_easy_strerror(res));
    }
    if (status < 200 || status >= 300) {
        throw runtime_error("upstream returned HTTP " + to_string(status));
    }
    return response;
}

static const char* kBase = "https://suis.sabanciuniv.edu/prod/";

static vector<string> parseSubjectList(const string& csv)
{
    vector<string> subjects;
    stringstream ss(csv);
    string item;
    while (getline(ss, item, ',')) {
        size_t b = item.find_first_not_of(" \t\r\n");
        size_t e = item.find_last_not_of(" \t\r\n");
        if (b == string::npos) continue;
        string code = item.substr(b, e - b + 1);
        transform(code.begin(), code.end(), code.begin(),
                  [](unsigned char c) { return toupper(c); });
        subjects.push_back(code);
    }
    return subjects;
}

static string fetchScheduleHtml(CURL* enc, const string& term,
                                const vector<string>& subjects)
{
    string body = "term_in=" + urlEncode(enc, term);
    body +=
        "&sel_subj=dummy&sel_day=dummy&sel_schd=dummy&sel_insm=dummy"
        "&sel_camp=dummy&sel_levl=dummy&sel_sess=dummy&sel_instr=dummy"
        "&sel_ptrm=dummy&sel_attr=dummy";
    for (const string& code : subjects) {
        body += "&sel_subj=" + urlEncode(enc, code);
    }
    body +=
        "&sel_crse=&sel_title=&sel_from_cred=&sel_to_cred="
        "&begin_hh=0&begin_mi=0&begin_ap=a&end_hh=0&end_mi=0&end_ap=a";
    return httpRequest(string(kBase) + "bwckschd.p_get_crse_unsec", body);
}

static string fetchTermsHtml()
{
    return httpRequest(string(kBase) + "bwckschd.p_disp_dyn_sched", "");
}

static string fetchSubjectsHtml(CURL* enc, const string& term)
{
    string body = "p_calling_proc=bwckschd.p_disp_dyn_sched"
                  "&p_term=" + urlEncode(enc, term);
    return httpRequest(string(kBase) + "bwckgens.p_proc_term_date", body);
}

struct Meeting {
    string type, time, days, where, dateRange, scheduleType, instructors;
};

struct Course {
    string title, crn, subject, course, section, credits;
    vector<Meeting> meetings;
};

struct Option {
    string value, label;
};

static string htmlToText(const string& html)
{
    string noTags;
    noTags.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) noTags += c;
    }

    struct { const char* from; const char* to; } entities[] = {
        {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"},
    };
    for (auto& e : entities) {
        string from = e.from;
        size_t pos = 0;
        while ((pos = noTags.find(from, pos)) != string::npos) {
            noTags.replace(pos, from.size(), e.to);
            pos += strlen(e.to);
        }
    }

    string out;
    out.reserve(noTags.size());
    bool prevSpace = false;
    for (char c : noTags) {
        if (isspace((unsigned char)c)) {
            if (!prevSpace && !out.empty()) out += ' ';
            prevSpace = true;
        } else {
            out += c;
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static vector<Option> parseSelectOptions(const string& html,
                                         const string& selectName)
{
    vector<Option> options;
    size_t nameAt = html.find("name=\"" + selectName + "\"");
    if (nameAt == string::npos) return options;
    size_t end = html.find("</select>", nameAt);
    string region = html.substr(nameAt, end == string::npos
                                             ? string::npos
                                             : end - nameAt);

    static const regex optRe(
        R"rx(<option\s+value="([^"]*)"[^>]*>([\s\S]*?)</option>)rx", regex::icase);
    for (auto it = sregex_iterator(region.begin(), region.end(), optRe);
         it != sregex_iterator(); ++it) {
        string value = (*it)[1].str();
        if (value.empty()) continue;
        options.push_back({value, htmlToText((*it)[2].str())});
    }
    return options;
}

static void parseTitleLine(const string& line, Course& c)
{
    vector<string> parts;
    const string sep = " - ";
    size_t start = 0, hit;
    while ((hit = line.find(sep, start)) != string::npos) {
        parts.push_back(line.substr(start, hit - start));
        start = hit + sep.size();
    }
    parts.push_back(line.substr(start));

    if (parts.size() >= 4) {
        c.section = parts.back();
        string subjCrse = parts[parts.size() - 2];
        c.crn = parts[parts.size() - 3];
        for (size_t i = 0; i + 3 < parts.size(); ++i) {
            if (i) c.title += sep;
            c.title += parts[i];
        }
        size_t sp = subjCrse.find(' ');
        if (sp != string::npos) {
            c.subject = subjCrse.substr(0, sp);
            c.course = subjCrse.substr(sp + 1);
        } else {
            c.subject = subjCrse;
        }
    } else {
        c.title = line;
    }
}

static vector<Meeting> parseMeetings(const string& block)
{
    vector<Meeting> meetings;
    size_t tablePos = block.find("Scheduled Meeting Times");
    if (tablePos == string::npos) return meetings;
    size_t tableEnd = block.find("</table>", tablePos);
    string region = block.substr(tablePos, tableEnd == string::npos
                                                ? string::npos
                                                : tableEnd - tablePos);

    static const regex rowRe(R"(<tr[^>]*>([\s\S]*?)</tr>)", regex::icase);
    static const regex cellRe(R"(<td[^>]*>([\s\S]*?)</td>)", regex::icase);

    for (auto it = sregex_iterator(region.begin(), region.end(), rowRe);
         it != sregex_iterator(); ++it) {
        string row = (*it)[1].str();
        vector<string> cells;
        for (auto c = sregex_iterator(row.begin(), row.end(), cellRe);
             c != sregex_iterator(); ++c) {
            cells.push_back(htmlToText((*c)[1].str()));
        }
        if (cells.empty()) continue;
        auto at = [&](size_t i) { return i < cells.size() ? cells[i] : ""; };
        meetings.push_back({at(0), at(1), at(2), at(3), at(4), at(5), at(6)});
    }
    return meetings;
}

static vector<Course> parseCourses(const string& html)
{
    vector<Course> courses;
    static const regex titleRe(
        R"(ddlabel[^>]*>\s*<a[^>]*>([^<]*)</a>)", regex::icase);
    static const regex creditsRe(R"(([0-9]+\.[0-9]+)\s*Credits)", regex::icase);

    vector<pair<size_t, string>> heads;
    for (auto it = sregex_iterator(html.begin(), html.end(), titleRe);
         it != sregex_iterator(); ++it) {
        heads.push_back({(size_t)it->position() + it->length(),
                         htmlToText((*it)[1].str())});
    }

    for (size_t i = 0; i < heads.size(); ++i) {
        size_t start = heads[i].first;
        size_t end = (i + 1 < heads.size()) ? heads[i + 1].first : html.size();
        string block = html.substr(start, end - start);

        Course c;
        parseTitleLine(heads[i].second, c);
        smatch m;
        if (regex_search(block, m, creditsRe)) c.credits = m[1].str();
        c.meetings = parseMeetings(block);
        courses.push_back(move(c));
    }
    return courses;
}

struct Json {
    enum Type { Str, Num, Arr, Obj } type = Str;
    string scalar;
    vector<Json> items;
    vector<pair<string, Json>> members;

    static Json str(const string& s) { Json j; j.type = Str; j.scalar = s; return j; }
    static Json num(long long n)     { Json j; j.type = Num; j.scalar = to_string(n); return j; }
    static Json arr()                { Json j; j.type = Arr; return j; }
    static Json obj()                { Json j; j.type = Obj; return j; }

    Json& set(const string& k, Json v) { members.emplace_back(k, move(v)); return *this; }
    Json& push(Json v)                 { items.push_back(move(v)); return *this; }

    static string escape(const string& s)
    {
        string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out + '"';
    }

    void dump(ostream& os, bool pretty, int depth = 0) const
    {
        const string nl = pretty ? "\n" : "";
        auto ind = [&](int d) { return pretty ? string(d * 2, ' ') : string(); };
        const string colon = pretty ? ": " : ":";
        switch (type) {
            case Num: os << scalar; break;
            case Str: os << escape(scalar); break;
            case Arr:
                if (items.empty()) { os << "[]"; break; }
                os << "[" << nl;
                for (size_t i = 0; i < items.size(); ++i) {
                    os << ind(depth + 1);
                    items[i].dump(os, pretty, depth + 1);
                    os << (i + 1 < items.size() ? "," : "") << nl;
                }
                os << ind(depth) << "]";
                break;
            case Obj:
                if (members.empty()) { os << "{}"; break; }
                os << "{" << nl;
                for (size_t i = 0; i < members.size(); ++i) {
                    os << ind(depth + 1) << escape(members[i].first) << colon;
                    members[i].second.dump(os, pretty, depth + 1);
                    os << (i + 1 < members.size() ? "," : "") << nl;
                }
                os << ind(depth) << "}";
                break;
        }
    }

    string dump(bool pretty) const
    {
        ostringstream os;
        dump(os, pretty);
        os << (pretty ? "\n" : "");
        return os.str();
    }
};

static Json courseToJson(const Course& c)
{
    Json j = Json::obj();
    j.set("title", Json::str(c.title))
     .set("crn", Json::str(c.crn))
     .set("subject", Json::str(c.subject))
     .set("course", Json::str(c.course))
     .set("section", Json::str(c.section))
     .set("credits", Json::str(c.credits));
    Json meetings = Json::arr();
    for (const Meeting& m : c.meetings) {
        Json mj = Json::obj();
        mj.set("type", Json::str(m.type))
          .set("time", Json::str(m.time))
          .set("days", Json::str(m.days))
          .set("where", Json::str(m.where))
          .set("dateRange", Json::str(m.dateRange))
          .set("scheduleType", Json::str(m.scheduleType))
          .set("instructors", Json::str(m.instructors));
        meetings.push(move(mj));
    }
    j.set("meetings", move(meetings));
    return j;
}

static Json coursesEnvelope(const string& term, const vector<string>& subjects,
                            const vector<Course>& courses)
{
    Json subjArr = Json::arr();
    for (const string& s : subjects) subjArr.push(Json::str(s));
    Json arr = Json::arr();
    for (const Course& c : courses) arr.push(courseToJson(c));

    Json env = Json::obj();
    env.set("term", Json::str(term))
       .set("subjects", move(subjArr))
       .set("count", Json::num((long long)courses.size()))
       .set("courses", move(arr));
    return env;
}

static Json optionsEnvelope(const string& key, const vector<Option>& opts,
                            const optional<string>& term = nullopt)
{
    Json arr = Json::arr();
    for (const Option& o : opts) {
        Json oj = Json::obj();
        oj.set("value", Json::str(o.value)).set("label", Json::str(o.label));
        arr.push(move(oj));
    }
    Json env = Json::obj();
    if (term) env.set("term", Json::str(*term));
    env.set("count", Json::num((long long)opts.size())).set(key, move(arr));
    return env;
}

static Json errorJson(const string& message)
{
    return Json::obj().set("error", Json::str(message));
}

static const size_t kMaxSubjects = 20;

static bool validTerm(const string& term)
{
    static const regex re("^[0-9]{6}$");
    return regex_match(term, re);
}

static bool validSubject(const string& code)
{
    static const regex re("^[A-Z]{1,8}$");
    return regex_match(code, re);
}

static bool isAll(const string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == string::npos) return false;
    string t = s.substr(b, e - b + 1);
    transform(t.begin(), t.end(), t.begin(),
              [](unsigned char c) { return tolower(c); });
    return t == "all";
}

class Cache {
public:
    optional<string> get(const string& key)
    {
        lock_guard<mutex> lk(m_);
        auto it = map_.find(key);
        if (it == map_.end()) return nullopt;
        if (chrono::steady_clock::now() >= it->second.expires) {
            map_.erase(it);
            return nullopt;
        }
        return it->second.body;
    }
    void put(const string& key, const string& body, chrono::seconds ttl)
    {
        lock_guard<mutex> lk(m_);
        map_[key] = {body, chrono::steady_clock::now() + ttl};
    }
private:
    struct Entry { string body; chrono::steady_clock::time_point expires; };
    mutex m_;
    unordered_map<string, Entry> map_;
};

class RateLimiter {
public:
    RateLimiter(int maxHits, chrono::seconds window)
        : max_(maxHits), window_(window) {}

    bool allow(const string& ip)
    {
        auto now = chrono::steady_clock::now();
        lock_guard<mutex> lk(m_);
        auto& w = windows_[ip];
        if (now >= w.start + window_) { w.start = now; w.count = 0; }
        if (w.count >= max_) return false;
        ++w.count;
        return true;
    }
private:
    struct Window { chrono::steady_clock::time_point start; int count = 0; };
    mutex m_;
    unordered_map<string, Window> windows_;
    int max_;
    chrono::seconds window_;
};

static void respond(httplib::Response& res, int status, const Json& body,
                    bool pretty)
{
    res.status = status;
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(body.dump(pretty), "application/json");
}

static string clientIp(const httplib::Request& req, bool behindProxy)
{
    if (behindProxy && req.has_header("X-Forwarded-For")) {
        string xff = req.get_header_value("X-Forwarded-For");
        size_t comma = xff.find(',');
        string first = (comma == string::npos) ? xff : xff.substr(0, comma);
        size_t b = first.find_first_not_of(" \t");
        size_t e = first.find_last_not_of(" \t");
        if (b != string::npos) return first.substr(b, e - b + 1);
    }
    return req.remote_addr;
}

static int runServer(const string& host, int port, bool behindProxy)
{
    httplib::Server svr;
    Cache cache;
    RateLimiter limiter(60, chrono::seconds(60));

    auto wantsPretty = [](const httplib::Request& req) {
        return req.has_param("pretty");
    };
    auto allowed = [&](const httplib::Request& req) {
        return limiter.allow(clientIp(req, behindProxy));
    };

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        respond(res, 200, Json::obj().set("status", Json::str("ok")), false);
    });

    svr.Get("/courses", [&](const httplib::Request& req, httplib::Response& res) {
        bool pretty = wantsPretty(req);
        if (!allowed(req)) {
            respond(res, 429, errorJson("rate limit exceeded"), pretty);
            return;
        }
        string term = req.get_param_value("term");
        string subjectsParam = req.get_param_value("subjects");
        if (term.empty() || subjectsParam.empty()) {
            respond(res, 400,
                    errorJson("required query params: term, subjects "
                              "(e.g. ?term=202503&subjects=CS,MATH or all)"),
                    pretty);
            return;
        }
        if (!validTerm(term)) {
            respond(res, 400, errorJson("term must be 6 digits, e.g. 202503"),
                    pretty);
            return;
        }

        bool wantAll = isAll(subjectsParam);
        vector<string> subjects;
        if (!wantAll) {
            subjects = parseSubjectList(subjectsParam);
            if (subjects.empty()) {
                respond(res, 400,
                        errorJson("no valid subject codes given"), pretty);
                return;
            }
            if (subjects.size() > kMaxSubjects) {
                respond(res, 400,
                        errorJson("too many subjects (max " +
                                  to_string(kMaxSubjects) + "); use 'all' "
                                  "for the full catalog"),
                        pretty);
                return;
            }
            for (const string& s : subjects) {
                if (!validSubject(s)) {
                    respond(res, 400,
                            errorJson("invalid subject code: " + s), pretty);
                    return;
                }
            }
        }

        try {
            if (wantAll) {
                string subjKey = "subjects:" + term;
                string subjHtml;
                if (auto cached = cache.get(subjKey)) {
                    subjHtml = *cached;
                } else {
                    Curl enc;
                    subjHtml = fetchSubjectsHtml(enc.get(), term);
                    cache.put(subjKey, subjHtml, chrono::hours(12));
                }
                for (const Option& o : parseSelectOptions(subjHtml, "sel_subj")) {
                    subjects.push_back(o.value);
                }
            }

            string key;
            chrono::seconds ttl;
            if (wantAll) {
                key = "sched:" + term + ":__all__";
                ttl = chrono::hours(6);
            } else {
                vector<string> sorted = subjects;
                sort(sorted.begin(), sorted.end());
                key = "sched:" + term + ":";
                for (const string& s : sorted) key += s + ",";
                ttl = chrono::hours(1);
            }

            string html;
            if (auto cached = cache.get(key)) {
                html = *cached;
            } else {
                Curl enc;
                html = fetchScheduleHtml(enc.get(), term, subjects);
                cache.put(key, html, ttl);
            }
            respond(res, 200,
                    coursesEnvelope(term, subjects, parseCourses(html)), pretty);
        } catch (const exception& e) {
            respond(res, 502, errorJson(e.what()), pretty);
        }
    });

    svr.Get("/terms", [&](const httplib::Request& req, httplib::Response& res) {
        bool pretty = wantsPretty(req);
        if (!allowed(req)) {
            respond(res, 429, errorJson("rate limit exceeded"), pretty);
            return;
        }
        try {
            string html;
            if (auto cached = cache.get("terms")) {
                html = *cached;
            } else {
                html = fetchTermsHtml();
                cache.put("terms", html, chrono::hours(24));
            }
            respond(res, 200,
                    optionsEnvelope("terms", parseSelectOptions(html, "p_term")),
                    pretty);
        } catch (const exception& e) {
            respond(res, 502, errorJson(e.what()), pretty);
        }
    });

    svr.Get("/subjects", [&](const httplib::Request& req, httplib::Response& res) {
        bool pretty = wantsPretty(req);
        if (!allowed(req)) {
            respond(res, 429, errorJson("rate limit exceeded"), pretty);
            return;
        }
        string term = req.get_param_value("term");
        if (term.empty()) {
            respond(res, 400,
                    errorJson("required query param: term (e.g. ?term=202503)"),
                    pretty);
            return;
        }
        if (!validTerm(term)) {
            respond(res, 400, errorJson("term must be 6 digits, e.g. 202503"),
                    pretty);
            return;
        }
        try {
            string key = "subjects:" + term;
            string html;
            if (auto cached = cache.get(key)) {
                html = *cached;
            } else {
                Curl enc;
                html = fetchSubjectsHtml(enc.get(), term);
                cache.put(key, html, chrono::hours(12));
            }
            respond(res, 200,
                    optionsEnvelope("subjects",
                                    parseSelectOptions(html, "sel_subj"), term),
                    pretty);
        } catch (const exception& e) {
            respond(res, 502, errorJson(e.what()), pretty);
        }
    });

    cout << "suAPI listening on http://" << host << ":" << port
         << (behindProxy ? " (trusting X-Forwarded-For)" : "") << "\n"
         << "  GET /courses?term=202503&subjects=CS,MATH\n"
         << "  GET /terms\n"
         << "  GET /subjects?term=202503\n"
         << "  GET /health\n";
    if (!svr.listen(host.c_str(), port)) {
        cerr << "error: could not bind " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}

static int cliCourses(const string& term, const string& subjectsCsv, bool pretty)
{
    Curl enc;
    vector<string> subjects;
    if (isAll(subjectsCsv)) {
        for (const Option& o :
             parseSelectOptions(fetchSubjectsHtml(enc.get(), term), "sel_subj")) {
            subjects.push_back(o.value);
        }
    } else {
        subjects = parseSubjectList(subjectsCsv);
    }
    if (subjects.empty()) {
        cout << errorJson("no valid subject codes given").dump(pretty);
        return 2;
    }
    string html = fetchScheduleHtml(enc.get(), term, subjects);
    cout << coursesEnvelope(term, subjects, parseCourses(html)).dump(pretty);
    return 0;
}

int main(int argc, char** argv)
{
    bool pretty = true;
    bool behindProxy = false;
    string host = "127.0.0.1";
    vector<string> args;
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--compact") pretty = false;
        else if (a == "--pretty") pretty = true;
        else if (a == "--behind-proxy") behindProxy = true;
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else args.push_back(a);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = 0;
    try {
        if (!args.empty() && args[0] == "serve") {
            int port = args.size() > 1 ? stoi(args[1]) : 8080;
            rc = runServer(host, port, behindProxy);
        } else if (!args.empty() && args[0] == "terms") {
            cout << optionsEnvelope("terms",
                     parseSelectOptions(fetchTermsHtml(), "p_term")).dump(pretty);
        } else if (!args.empty() && args[0] == "subjects" && args.size() > 1) {
            Curl enc;
            cout << optionsEnvelope("subjects",
                     parseSelectOptions(fetchSubjectsHtml(enc.get(), args[1]),
                                        "sel_subj"), args[1]).dump(pretty);
        } else if (args.size() >= 2) {
            rc = cliCourses(args[0], args[1], pretty);
        } else {
            cout << errorJson(
                "usage:\n"
                "  serve [port]            start HTTP API (default 8080)\n"
                "  <term> <subjects>       course sections, e.g. 202503 CS,MATH\n"
                "                          (subjects may be 'all' for every code)\n"
                "  terms                   list available terms\n"
                "  subjects <term>         list subjects for a term\n"
                "  flags: --compact | --pretty\n"
                "  serve flags: --host <addr> (default 127.0.0.1; use 0.0.0.0\n"
                "               for public), --behind-proxy (trust "
                "X-Forwarded-For)").dump(pretty);
            rc = 2;
        }
    } catch (const exception& e) {
        cout << errorJson(e.what()).dump(pretty);
        rc = 1;
    }
    curl_global_cleanup();
    return rc;
}
