#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <regex>

#include "module_scanner.hpp"
#include "dependency_graph.hpp"
#include "sha256.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#endif

#include <stdlib.h>
#include <thread>

using namespace cppm;

static inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static inline std::string sanitize_name(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (char c : in) {
        unsigned char uc = (unsigned char)c;
        if (std::isalnum(uc) || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    if (out.empty()) return std::string("pkg");
    // collapse multiple leading/trailing underscores? leave as-is
    return out;
}

struct Source { std::string name; std::string url; int priority = 100; };

static std::vector<Source> parse_cpm_sources(const std::filesystem::path& tomlPath) {
    std::vector<Source> out;
    if (!std::filesystem::exists(tomlPath)) return out;
    std::ifstream in(tomlPath);
    std::string line;
    Source cur; bool inArray = false;
    auto trim = [](std::string s){ size_t a = s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return std::string(); s = s.substr(a); size_t b = s.find_last_not_of(" \t\r\n"); if (b!=std::string::npos) s = s.substr(0,b+1); return s; };
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("[[source]]",0)==0) { if (inArray) out.push_back(cur); cur = Source(); inArray = true; continue; }
        if (!inArray) continue;
        if (line.rfind("name",0)==0) { auto p=line.find('='); if (p!=std::string::npos) { auto v=trim(line.substr(p+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') cur.name = v.substr(1,v.size()-2); } }
        if (line.rfind("url",0)==0) { auto p=line.find('='); if (p!=std::string::npos) { auto v=trim(line.substr(p+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') cur.url = v.substr(1,v.size()-2); } }
        if (line.rfind("priority",0)==0) { auto p=line.find('='); if (p!=std::string::npos) { auto v=trim(line.substr(p+1)); try { cur.priority = std::stoi(v); } catch(...) {} } }
    }
    if (inArray) out.push_back(cur);
    // sort by priority asc
    std::sort(out.begin(), out.end(), [](const Source&a,const Source&b){ return a.priority < b.priority; });
    return out;
}

static bool try_download_url_to(const std::string& url, const std::filesystem::path& dest) {
    // Prefer curl if available, fallback to wget. Return true on success.
    std::string cmd = "curl -fsSL -o \"" + dest.string() + "\" \"" + url + "\"";
    int rc = std::system(cmd.c_str());
    if (rc == 0 && std::filesystem::exists(dest)) return true;
    cmd = "wget -q -O \"" + dest.string() + "\" \"" + url + "\"";
    rc = std::system(cmd.c_str());
    if (rc == 0 && std::filesystem::exists(dest)) return true;
    return false;
}

static bool try_git_clone_repo(const std::string& repoUrl, const std::filesystem::path& dest, const std::string& tag="") {
    std::filesystem::remove_all(dest);
    std::string cmd;
    if (!tag.empty()) {
        cmd = "git clone --depth 1 --branch " + tag + " \"" + repoUrl + "\" \"" + dest.string() + "\"";
        if (std::system(cmd.c_str()) == 0) return std::filesystem::exists(dest);
        // try tag as v<tag>
        std::string vtag = std::string("v") + tag;
        cmd = "git clone --depth 1 --branch " + vtag + " \"" + repoUrl + "\" \"" + dest.string() + "\"";
        if (std::system(cmd.c_str()) == 0) return std::filesystem::exists(dest);
        return false;
    } else {
        cmd = "git clone --depth 1 \"" + repoUrl + "\" \"" + dest.string() + "\"";
        if (std::system(cmd.c_str()) == 0) return std::filesystem::exists(dest);
        return false;
    }
}

static std::optional<std::filesystem::path> fetch_from_sources(const std::string& name, const std::string& version, const std::vector<Source>& sources) {
    for (const auto& s : sources) {
        // try HTTP archive
        std::string archiveUrl = s.url;
        if (!archiveUrl.empty() && archiveUrl.back()=='/') archiveUrl.pop_back();
        archiveUrl += "/packages/" + name + "@" + version + ".tar.gz";
        auto tmp = std::filesystem::temp_directory_path() / ("cpm_fetch_" + name + "@" + version + "_" + std::to_string(s.priority));
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        auto dest = tmp / (name + "@" + version + ".tar.gz");
        if (try_download_url_to(archiveUrl, dest)) return dest;
        // HTTP failed, try git if url looks like a git host or starts with http
        std::string repoGuess = s.url;
        if (repoGuess.find("git://")==0 || repoGuess.find("http")==0 || repoGuess.find("ssh://")==0) {
            // try repo/name.git
            std::string repoUrl = repoGuess;
            if (repoUrl.back()=='/') repoUrl.pop_back();
            repoUrl += "/" + name + ".git";
            auto repoDest = tmp / "repo";
            if (try_git_clone_repo(repoUrl, repoDest, version)) {
                // create archive from repo
                auto archiveOut = tmp / (name + "@" + version + ".tar.gz");
                std::string tarcmd = "tar -C \"" + repoDest.string() + "\" -czf \"" + archiveOut.string() + "\" .";
                if (std::system(tarcmd.c_str())==0 && std::filesystem::exists(archiveOut)) return archiveOut;
            }
            // try cloning the base url as repo (if it ends with .git)
            if (repoGuess.rfind(".git") != std::string::npos) {
                auto repoDest2 = tmp / "repo2";
                if (try_git_clone_repo(repoGuess, repoDest2, version)) {
                    auto archiveOut = tmp / (name + "@" + version + ".tar.gz");
                    std::string tarcmd = "tar -C \"" + repoDest2.string() + "\" -czf \"" + archiveOut.string() + "\" .";
                    if (std::system(tarcmd.c_str())==0 && std::filesystem::exists(archiveOut)) return archiveOut;
                }
            }
        }
    }
    return std::nullopt;
}


std::string getEnvironmentVariable(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        std::string result(value);
        free(value);
        return result;
    }
    return std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

struct IndexEntry { std::string name; std::string version; std::filesystem::path path; std::string archive; std::string sha256; std::string timestamp; std::string metadata; };

static std::filesystem::path default_registry_root() {
    // Cross-platform home lookup: prefer HOME, fallback to POSIX getpwuid, else use current path
    std::string homeEnv = getEnvironmentVariable("HOME");
    std::string userProfile = getEnvironmentVariable("USERPROFILE");
#if defined(_WIN32)
    // On Windows, prefer USERPROFILE
    if (!userProfile.empty()) return std::filesystem::path(userProfile) / ".cppm" / "registry";
#endif
    if (!homeEnv.empty()) return std::filesystem::path(homeEnv) / ".cppm" / "registry";
#if defined(__unix__) || defined(__APPLE__)
    {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) return std::filesystem::path(pw->pw_dir) / ".cppm" / "registry";
    }
#endif
    // Fallback: use current working directory
    return std::filesystem::current_path() / ".cppm" / "registry";
}

struct PackageMetadata { std::string name; std::string version; std::string description; std::vector<std::string> dependencies; };

static std::optional<PackageMetadata> parse_toml_metadata(const std::filesystem::path& tomlPath) {
    if (!std::filesystem::exists(tomlPath)) return std::nullopt;
    std::ifstream in(tomlPath);
    std::string line;
    PackageMetadata meta;
    auto trim = [](std::string s){ size_t a = s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return std::string(); s = s.substr(a); size_t b = s.find_last_not_of(" \t\r\n"); if (b!=std::string::npos) s = s.substr(0,b+1); return s; };
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("name",0)==0) {
            auto pos = line.find('='); if (pos!=std::string::npos) { auto v = trim(line.substr(pos+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') meta.name = v.substr(1,v.size()-2); }
        }
        if (line.rfind("version",0)==0) {
            auto pos = line.find('='); if (pos!=std::string::npos) { auto v = trim(line.substr(pos+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') meta.version = v.substr(1,v.size()-2); }
        }
        if (line.rfind("description",0)==0) {
            auto pos = line.find('='); if (pos!=std::string::npos) { auto v = trim(line.substr(pos+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') meta.description = v.substr(1,v.size()-2); }
        }
        if (line.rfind("dependencies",0)==0 || line.rfind("deps",0)==0) {
            auto pos = line.find('='); if (pos!=std::string::npos) {
                auto v = trim(line.substr(pos+1));
                // expect ["a@1","b@2"] or [a,b]
                if (!v.empty() && v.front()=='[' && v.back()==']') {
                    v = v.substr(1, v.size()-2);
                    std::istringstream ss(v);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        token = trim(token);
                        if (!token.empty() && token.front()=='"' && token.back()=='"') token = token.substr(1, token.size()-2);
                        if (!token.empty()) meta.dependencies.push_back(token);
                    }
                }
            }
        }
    }
    if (meta.name.empty()) return std::nullopt;
    if (meta.version.empty()) meta.version = "0.0.0";
    return meta;
}

static bool generate_cppmod_toml(const std::filesystem::path& dir) {
    // don't overwrite existing cppmod.toml
    auto outPath = dir / "cppmod.toml";
    if (std::filesystem::exists(outPath)) return false;

    std::string name;
    std::string version = "0.0.0";
    std::string description;
    std::set<std::string> deps;

    auto trim_local = [](std::string s){ size_t a = s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return std::string(); size_t b = s.find_last_not_of(" \t\r\n"); if (b!=std::string::npos) return s.substr(a, b-a+1); return s.substr(a); };

    // 1) try pyproject.toml for metadata
    auto py = dir / "pyproject.toml";
    if (std::filesystem::exists(py)) {
        std::ifstream in(py);
        std::string line;
        bool in_project = false, in_poetry = false;
        while (std::getline(in, line)) {
            line = trim_local(line);
            if (line.rfind("[project]",0)==0) { in_project=true; in_poetry=false; continue; }
            if (line.rfind("[tool.poetry]",0)==0) { in_poetry=true; in_project=false; continue; }
            if (!line.empty() && line.front()=='[') { in_project=false; in_poetry=false; }
            if ((in_project || in_poetry) && line.rfind("name",0)==0) {
                auto p = line.find('='); if (p!=std::string::npos) { auto v = trim_local(line.substr(p+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') name = v.substr(1,v.size()-2); }
            }
            if ((in_project || in_poetry) && line.rfind("version",0)==0) {
                auto p = line.find('='); if (p!=std::string::npos) { auto v = trim_local(line.substr(p+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') version = v.substr(1,v.size()-2); }
            }
            if (description.empty() && line.rfind("description",0)==0) {
                auto p = line.find('='); if (p!=std::string::npos) { auto v = trim_local(line.substr(p+1)); if (!v.empty() && v.front()=='"' && v.back()=='"') description = v.substr(1,v.size()-2); }
            }
            if (!name.empty() && !version.empty()) break;
        }
    }

    // 2) try CMakeLists.txt project(...) declaration
    if (name.empty()) {
        auto cm = dir / "CMakeLists.txt";
        if (std::filesystem::exists(cm)) {
            std::ifstream in(cm); std::string line;
            std::regex proj_rx("project\\s*\\(\\s*([^ )\\n\\r]+)(?:\\s+VERSION\\s+([^ )\\n\\r]+))?", std::regex_constants::icase);
            while (std::getline(in, line)) {
                std::smatch m; if (std::regex_search(line, m, proj_rx)) {
                    if (m.size() >= 2) {
                        name = m[1].str();
                        if (m.size() >= 3 && m[2].matched) version = m[2].str();
                        break;
                    }
                }
            }
        }
    }

    // 3) fallback to directory name
    if (name.empty()) {
        try {
            auto abs = std::filesystem::absolute(dir);
            name = abs.filename().string();
        } catch(...) { name = dir.filename().string(); }
    }
    // sanitize package name: replace illegal chars with '_'
    name = sanitize_name(name);
    // ensure name isn't just underscores
    bool all_underscore = true; for (char c : name) if (c != '_') { all_underscore = false; break; }
    if (all_underscore) name = std::string("pkg_") + sanitize_name(dir.filename().string());

    // 4) description from README
    if (description.empty()) {
        auto rd = dir / "README.md";
        if (std::filesystem::exists(rd)) {
            std::ifstream in(rd); std::string line;
            while (std::getline(in, line)) { line = trim_local(line); if (!line.empty()) { description = line; break; } }
        }
    }

    // 5) detect dependencies by scanning source files for 'import' and includes with paths
    std::vector<std::string> exts = {".cpp",".cc",".cxx",".c",".ixx",".cppm",".h",".hpp"};
    for (auto &it : std::filesystem::recursive_directory_iterator(dir)) {
        if (!it.is_regular_file()) continue;
        auto p = it.path(); auto e = p.extension().string(); bool ok=false; for (auto &x: exts) if (e==x) { ok=true; break; } if (!ok) continue;
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) {
            auto t = trim_local(line);
            if (t.rfind("import ",0)==0) {
                // import foo; or import foo.bar;
                auto rest = t.substr(7);
                auto semi = rest.find(';'); if (semi!=std::string::npos) rest = rest.substr(0,semi);
                rest = trim_local(rest);
                if (!rest.empty()) {
                    // take first segment before '.' or '::' or '<'
                    auto pos = rest.find_first_of(".::<"); if (pos!=std::string::npos) rest = rest.substr(0,pos);
                    deps.insert(rest);
                }
            }
            if (t.rfind("#include",0)==0) {
                auto posq = t.find('"'); auto posa = t.find('<'); std::string inc;
                if (posq!=std::string::npos) { auto p2 = t.find('"', posq+1); if (p2!=std::string::npos) inc = t.substr(posq+1, p2-posq-1); }
                else if (posa!=std::string::npos) { auto p2 = t.find('>', posa+1); if (p2!=std::string::npos) inc = t.substr(posa+1, p2-posa-1); }
                if (!inc.empty()) {
                    auto slash = inc.find('/');
                    if (slash!=std::string::npos) {
                        auto candidate = inc.substr(0, slash);
                        if (candidate != name) deps.insert(candidate);
                    }
                }
            }
        }
    }

    // remove common system headers (heuristic: entries with no alnum or containing '.')
    std::set<std::string> filtered;
    for (auto &d : deps) {
        if (d.empty()) continue;
        bool ok=true; for (char c: d) if (!std::isalnum((unsigned char)c) && c!='_' && c!='-' ) { ok=false; break; }
        if (ok && d!=name) filtered.insert(d);
    }

    // write cppmod.toml
    std::ofstream out(outPath, std::ios::trunc);
    if (!out) return false;
    out << "name = \"" << name << "\"\n";
    out << "version = \"" << version << "\"\n";
    if (!description.empty()) out << "description = \"" << description << "\"\n";
    out << "dependencies = [";
    size_t i=0; for (auto &d : filtered) { if (i++) out << ", "; out << "\"" << d << "\""; }
    out << "]\n";
    out.close();
    return true;
}

// Registry stored as a single JSON array file (registry.json)
static std::vector<IndexEntry> load_index(const std::filesystem::path& indexFile) {
    std::vector<IndexEntry> out;
    if (!std::filesystem::exists(indexFile)) return out;
    std::string content;
    {
        std::ifstream in(indexFile, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf(); content = ss.str();
    }
    // find object occurrences by searching for {"name":
    size_t pos = 0;
    while (true) {
        auto p = content.find('{', pos);
        if (p == std::string::npos) break;
        auto q = content.find('}', p);
        if (q == std::string::npos) break;
        std::string obj = content.substr(p, q - p + 1);
        auto find_str = [&](const std::string& key)->std::string{
            auto r = obj.find('"'+key+'"'); if (r==std::string::npos) return {};
            auto s = obj.find(':', r); if (s==std::string::npos) return {};
            auto a = obj.find('"', s); if (a==std::string::npos) return {};
            auto b = obj.find('"', a+1); if (b==std::string::npos) return {};
            return obj.substr(a+1, b-(a+1));
        };
        IndexEntry e; e.name = find_str("name"); e.version = find_str("version"); e.path = find_str("path"); e.archive = find_str("archive"); e.sha256 = find_str("sha256"); e.timestamp = find_str("timestamp"); e.metadata = find_str("metadata");
        if (!e.name.empty()) out.push_back(e);
        pos = q + 1;
    }
    return out;
}

static bool acquire_lock(const std::filesystem::path& lockPath, int timeoutMs = 5000) {
    using namespace std::chrono;
    auto start = steady_clock::now();
    while (std::filesystem::exists(lockPath)) {
        if (duration_cast<milliseconds>(steady_clock::now() - start).count() > timeoutMs) return false;
        std::this_thread::sleep_for(milliseconds(50));
    }
    // create lock
    std::ofstream lock(lockPath);
    if (!lock) return false;
    lock << std::this_thread::get_id();
    lock.close();
    return true;
}

static void release_lock(const std::filesystem::path& lockPath) {
    std::error_code ec; std::filesystem::remove(lockPath, ec);
}

static void save_index(const std::filesystem::path& indexFile, const std::vector<IndexEntry>& entries) {
    auto lockPath = indexFile;
    lockPath += ".lock";
    if (!acquire_lock(lockPath, 5000)) throw std::runtime_error("Could not acquire registry lock");
    std::filesystem::path tmp = indexFile;
    tmp += ".tmp";

    std::ofstream out(tmp, std::ios::trunc);
    out << "[\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        out << "  {\"name\":\"" << e.name << "\",\"version\":\"" << e.version << "\",\"path\":\"" << e.path.generic_string() << "\",\"archive\":\"" << e.archive << "\",\"sha256\":\"" << e.sha256 << "\",\"timestamp\":\"" << e.timestamp << "\",\"metadata\":\"" << e.metadata << "\"}";
        if (i + 1 < entries.size()) out << ",\n";
        else out << "\n";
    }
    out << "]\n";
    out.close();
    std::filesystem::rename(tmp, indexFile);
    release_lock(lockPath);
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: cpm <command> [args...]\n";
                        std::cerr << "Commands: init, list, search <term>, install <path>, remove <name@ver>, scan <paths...>, autogen <path?>\n";
            return 1;
        }

        std::string cmd = argv[1];
        auto registry = default_registry_root();
        auto indexFile = registry / "registry.json"; // NDJSON: one JSON object per line

        if (cmd == "init") {
            std::filesystem::create_directories(registry);
            if (!std::filesystem::exists(indexFile)) {
                save_index(indexFile, {});
            }
            std::cout << "Initialized registry at " << registry.string() << "\n";
            return 0;
        }

        if (cmd == "list") {
            auto idx = load_index(indexFile);
            for (auto& e : idx) std::cout << e.name << "@" << e.version << "  " << e.path.string() << "\n";
            return 0;
        }

        if (cmd == "search") {
            if (argc < 3) { std::cerr << "search requires a term (name or name@version)\n"; return 1; }
            std::string term = argv[2];
            auto idx = load_index(indexFile);
            if (term.find('@') != std::string::npos) {
                // exact
                for (auto& e : idx) if ((e.name + "@" + e.version) == term) std::cout << e.name << "@" << e.version << "  " << e.archive << "\n";
            } else {
                // fuzzy: list versions
                std::map<std::string, std::vector<std::string>> m;
                for (auto& e : idx) if (e.name.find(term) != std::string::npos) m[e.name].push_back(e.version);
                for (auto &p : m) {
                    std::cout << p.first << " -> ";
                    for (size_t i=0;i<p.second.size();++i) { std::cout << p.second[i]; if (i+1<p.second.size()) std::cout << ", "; }
                    std::cout << "\n";
                }
            }
            return 0;
        }

        if (cmd == "uninstall") {
            if (argc < 2) { std::cerr << "uninstall requires name@version\n"; return 1; }
            std::string spec = argv[2]; auto pos = spec.find('@'); if (pos==std::string::npos) { std::cerr << "spec must be name@version\n"; return 1; }
            std::string name = spec.substr(0,pos); std::string ver = spec.substr(pos+1);
            auto idx = load_index(indexFile);
            std::vector<IndexEntry> remaining; bool found=false;
            for (auto &e : idx) {
                if (e.name==name && e.version==ver && std::filesystem::exists(e.path) && std::filesystem::is_directory(e.path)) {
                    std::filesystem::remove_all(e.path);
                    found = true;
                } else remaining.push_back(e);
            }
            if (!found) { std::cerr << "installed package not found\n"; return 1; }
            save_index(indexFile, remaining);
            std::cout << "Uninstalled " << spec << "\n";
            return 0;
        }

        if (cmd == "export-lock") {
            // If cpm.toml exists in current dir, resolve declared dependencies to produce full lockfile
            auto projectToml = std::filesystem::current_path() / "cpm.toml";
            auto lockPath = registry / "lock.json";
            if (!std::filesystem::exists(projectToml)) {
                // Fallback: Export installed packages and metadata to registry/lock.json
                auto idx = load_index(indexFile);
                std::vector<IndexEntry> installed;
                for (auto &e : idx) if (std::filesystem::exists(e.path) && std::filesystem::is_directory(e.path)) installed.push_back(e);
                std::ofstream out(lockPath, std::ios::trunc);
                out << "{\"packages\": [\n";
                for (size_t i=0;i<installed.size();++i) {
                    auto &e = installed[i];
                    out << "  {\"name\":\"" << e.name << "\",\"version\":\"" << e.version << "\",\"archive\":\"" << e.archive << "\",\"sha256\":\"" << e.sha256 << "\",\"metadata\":\"" << e.metadata << "\"}";
                    if (i+1<installed.size()) out << ",\n"; else out << "\n";
                }
                out << "]}\n";
                out.close();
                std::cout << "Wrote lock file to " << lockPath.string() << "\n";
                return 0;
            }

            // Parse project versions and dependencies
            std::map<std::string,std::string> versionsMap;
            std::map<std::string,std::string> depsMap; // dep name -> spec (could be ref:... or ver or name@ver)
            {
                std::ifstream in(projectToml);
                std::string line;
                enum Section { NoneS, Versions, Dependencies, Sources } sec = NoneS;
                auto trim = [](std::string s){ size_t a = s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return std::string(); s = s.substr(a); size_t b = s.find_last_not_of(" \t\r\n"); if (b!=std::string::npos) s = s.substr(0,b+1); return s; };
                while (std::getline(in, line)) {
                    line = trim(line);
                    if (line.empty()) continue;
                    if (line.rfind("[versions]",0)==0) { sec = Versions; continue; }
                    if (line.rfind("[dependencies]",0)==0) { sec = Dependencies; continue; }
                    if (line.rfind("[registry]",0)==0) { sec = NoneS; continue; }
                    if (sec == Versions) {
                        auto p = line.find('='); if (p!=std::string::npos) {
                            auto key = trim(line.substr(0,p)); auto val = trim(line.substr(p+1));
                            if (!key.empty() && !val.empty()) {
                                if (key.front()=='"' && key.back()=='"') key = key.substr(1,key.size()-2);
                                if (val.front()=='"' && val.back()=='"') val = val.substr(1,val.size()-2);
                                versionsMap[key]=val;
                            }
                        }
                    } else if (sec == Dependencies) {
                        auto p = line.find('='); if (p!=std::string::npos) {
                            auto key = trim(line.substr(0,p)); auto val = trim(line.substr(p+1));
                            if (!key.empty() && key.front()=='"' && key.back()=='"') key = key.substr(1,key.size()-2);
                            // val may be a table: { version.ref = "fmt" } or a simple string
                            if (val.rfind("{",0)==0) {
                                // find version.ref
                                auto pos = val.find("version.ref");
                                if (pos!=std::string::npos) {
                                    auto eq = val.find('=', pos);
                                    if (eq!=std::string::npos) {
                                        auto ref = trim(val.substr(eq+1)); if (ref.front()=='"' && ref.back()=='"') ref = ref.substr(1,ref.size()-2);
                                        depsMap[key] = std::string("ref:") + ref;
                                    }
                                }
                            } else {
                                // simple string
                                if (val.front()=='"' && val.back()=='"') val = val.substr(1,val.size()-2);
                                depsMap[key] = val;
                            }
                        }
                    }
                }
            }

            // Load registry index to pick versions when unspecified
            auto idx = load_index(indexFile);
            auto find_latest = [&](const std::string& pkgName)->std::optional<std::string>{
                std::string best;
                for (auto &e : idx) if (e.name==pkgName) {
                    if (best.empty() || e.version > best) best = e.version;
                }
                if (best.empty()) return std::nullopt; return best;
            };

            // Resolve deps transitively
            std::map<std::string, IndexEntry> resolved; // name -> entry
            std::deque<std::pair<std::string,std::string>> q; // (name, version)
            for (auto &p : depsMap) {
                std::string name = p.first; std::string spec = p.second;
                if (spec.rfind("ref:",0)==0) {
                    auto ref = spec.substr(4);
                    if (versionsMap.contains(ref)) q.push_back(std::make_pair(name, versionsMap[ref]));
                    else { std::cerr << "Undefined version ref: " << ref << "\n"; }
                } else if (spec.find('@')!=std::string::npos) {
                    auto at = spec.find('@'); q.push_back(std::make_pair(name, spec.substr(at+1)));
                } else if (!spec.empty()) {
                    // spec is version
                    q.push_back(std::make_pair(name, spec));
                } else {
                    auto v = find_latest(name);
                    if (v) q.push_back(std::make_pair(name, *v));
                    else std::cerr << "Could not find version for dependency " << name << "\n";
                }
            }

            while (!q.empty()) {
                auto cur = q.front(); q.pop_front();
                auto name = cur.first; auto ver = cur.second;
                if (resolved.contains(name)) continue;
                // find in idx
                IndexEntry chosen; bool foundEntry=false;
                for (auto &e : idx) if (e.name==name && e.version==ver) { chosen = e; foundEntry = true; break; }
                if (!foundEntry) {
                    std::cerr << "Dependency not found in registry: " << name << "@" << ver << "\n";
                    continue;
                }
                resolved[name] = chosen;
                // parse its metadata JSON to enqueue transitive deps
                if (!chosen.metadata.empty()) {
                    auto m = chosen.metadata;
                    // find "dependencies":[...]
                    auto pos = m.find("\"dependencies\"");
                    if (pos!=std::string::npos) {
                        auto b = m.find('[', pos);
                        auto e = m.find(']', b);
                        if (b!=std::string::npos && e!=std::string::npos) {
                            auto body = m.substr(b+1, e-b-1);
                            std::istringstream ss(body); std::string tok;
                            while (std::getline(ss, tok, ',')) {
                                auto t = trim(tok);
                                if (t.front()=='"' && t.back()=='"') t = t.substr(1,t.size()-2);
                                if (t.empty()) continue;
                                // t may be name@ver or name
                                if (t.find('@')!=std::string::npos) {
                                    auto at = t.find('@'); q.push_back(std::make_pair(t.substr(0,at), t.substr(at+1)));
                                } else {
                                    auto v = find_latest(t);
                                    if (v) q.push_back(std::make_pair(t, *v));
                                }
                            }
                        }
                    }
                }
            }

            // write lock.json
            std::ofstream out(lockPath, std::ios::trunc);
            out << "{\"locked\": [\n";
            size_t i=0; for (auto &kv : resolved) {
                auto &e = kv.second;
                out << "  {\"name\":\"" << e.name << "\",\"version\":\"" << e.version << "\",\"archive\":\"" << e.archive << "\",\"sha256\":\"" << e.sha256 << "\"}";
                if (++i < resolved.size()) out << ",\n"; else out << "\n";
            }
            out << "]}\n";
            out.close();
            std::cout << "Wrote lock file to " << lockPath.string() << "\n";
            return 0;
        }

        if (cmd == "pack") {
            if (argc < 3) { std::cerr << "pack requires a path or name@version\n"; return 1; }
            std::string arg = argv[2];
            std::filesystem::path src;
            // if arg looks like name@version and not an existing path, treat as registry spec
            if (arg.find('@') != std::string::npos && !std::filesystem::exists(arg)) {
                std::string name = arg.substr(0, arg.find('@'));
                std::string ver = arg.substr(arg.find('@')+1);
                auto idx = load_index(indexFile);
                std::optional<IndexEntry> found;
                for (auto &ie : idx) if (ie.name==name && ie.version==ver) { found = ie; break; }
                if (!found) { std::cerr << "package not found in registry: " << arg << "\n"; return 1; }
                src = found->path;
                if (!std::filesystem::exists(src) || !std::filesystem::is_directory(src)) { std::cerr << "package files missing: " << src.string() << "\n"; return 1; }
            } else {
                src = std::filesystem::path(arg);
                if (!std::filesystem::exists(src) || !std::filesystem::is_directory(src)) { std::cerr << "package path must be an existing directory\n"; return 1; }
            }

            // Ensure cppmod.toml exists (generate if needed)
            if (!std::filesystem::exists(src / "cppmod.toml")) {
                if (!generate_cppmod_toml(src)) {
                    std::cerr << "cppmod.toml missing and autogen failed\n"; return 1;
                }
            }
            auto meta = parse_toml_metadata(src / "cppmod.toml");
            if (!meta) { std::cerr << "cppmod.toml missing or invalid after generation\n"; return 1; }
            auto name = meta->name; auto version = meta->version;
            std::string archiveName = name + "@" + version + ".tar.gz";
            auto outPath = std::filesystem::current_path() / archiveName;
            // Read .cppmignore (if any) and build exclude arguments
            std::vector<std::string> ignores;
            auto ignorePath = src / ".cppmignore";
            if (std::filesystem::exists(ignorePath)) {
                std::ifstream ig(ignorePath);
                std::string il;
                while (std::getline(ig, il)) {
                    auto line = trim(il);
                    if (line.empty()) continue;
                    if (line.front() == '#') continue;
                    ignores.push_back(line);
                }
            }
            // default ignores to supplement
            std::vector<std::string> defaults = {"build/","cmake-build-*/",".git/","*.ifc","*.pcm","*.gcm","*.exe","*.dll","*.so"};
            for (auto &d : defaults) if (std::find(ignores.begin(), ignores.end(), d) == ignores.end()) ignores.push_back(d);

            std::string excludeArgs;
            for (auto &p : ignores) {
                // escape quotes if any
                std::string pat = p;
                size_t pos = 0; while ((pos = pat.find('"', pos)) != std::string::npos) { pat.replace(pos,1,"\\\""); pos += 2; }
                excludeArgs += " --exclude=\"" + pat + "\"";
            }

            // Create tar.gz using system tar with excludes
            std::string cmdline = "tar -C \"" + src.string() + "\"" + excludeArgs + " -czf \"" + outPath.string() + "\" .";
            std::cout << "Running: " << cmdline << "\n";
            if (std::system(cmdline.c_str()) != 0) { std::cerr << "tar failed\n"; return 1; }
            std::cout << "Created archive: " << outPath.string() << "\n";
            return 0;
        }

        if (cmd == "publish") {
            if (argc < 3) { std::cerr << "publish requires path to archive (.tar.gz)\n"; return 1; }
            std::filesystem::path archive = argv[2];
            if (!std::filesystem::exists(archive) || !archive.has_extension()) { std::cerr << "archive not found\n"; return 1; }
            std::filesystem::create_directories(registry / "packages");
            auto dest = registry / "packages" / archive.filename();
            std::filesystem::copy_file(archive, dest, std::filesystem::copy_options::overwrite_existing);
            // compute sha256
            std::ifstream in(dest, std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            auto sha = cppm::sha256_hex(ss.str());

                        // extract cppmod.toml from archive (into tmp) to read metadata
                        auto tmpdir = std::filesystem::temp_directory_path() / ("cpm_pub_" + dest.filename().string());
                        std::filesystem::remove_all(tmpdir);
                        std::filesystem::create_directories(tmpdir);
                        std::string extractCmd = "tar -C \"" + tmpdir.string() + "\" -xzf \"" + dest.string() + "\" cppmod.toml 2>nul";
                        std::system(extractCmd.c_str());
                        std::string metadata;
                        auto meta = parse_toml_metadata(tmpdir / "cppmod.toml");
                        if (meta) {
                            // record description and dependencies in metadata field (JSON-escaped simple)
                            std::ostringstream mss; mss << "{";
                            mss << "\"description\":\"" << meta->description << "\"";
                            if (!meta->dependencies.empty()) {
                                mss << ",\"dependencies\":[";
                                for (size_t i=0;i<meta->dependencies.size();++i) {
                                    mss << "\"" << meta->dependencies[i] << "\"";
                                    if (i+1<meta->dependencies.size()) mss << ",";
                                }
                                mss << "]";
                            }
                            mss << "}";
                            metadata = mss.str();
                        }

                        // timestamp
                        auto now = std::chrono::system_clock::now();
                        std::time_t t = std::chrono::system_clock::to_time_t(now);
                        char buf[64];
            #if defined(_MSC_VER)
                std::tm tm{};
                gmtime_s(&tm, &t);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            #else
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
            #endif

                        // add to registry index
                        auto idx = load_index(indexFile);
                        // parse name@ver from filename if possible
                        std::string fname = archive.filename().string();
                        std::string base = fname;
                        if (base.rfind(".tar.gz") != std::string::npos) base = base.substr(0, base.size()-7);
                        auto at = base.find('@'); if (at==std::string::npos) { std::cerr << "archive name must be name@version.tar.gz\n"; return 1; }
                        std::string name = base.substr(0, at); std::string version = base.substr(at+1);
                        IndexEntry e; e.name = name; e.version = version; e.path = dest.string(); e.archive = dest.string(); e.sha256 = sha; e.timestamp = buf; e.metadata = metadata;
                        idx.push_back(e);
                        save_index(indexFile, idx);
                        std::cout << "Published: " << name << "@" << version << " sha256=" << sha << "\n";
                        return 0;
                    }

        if (cmd == "install") {
            // install can accept either a local package dir or a registry spec name@version
            if (argc < 3) { std::cerr << "install requires a path or name@version\n"; return 1; }
            std::string arg = argv[2];
            if (arg.find('@') != std::string::npos && !std::filesystem::exists(arg)) {
                // treat as registry spec
                std::string name = arg.substr(0, arg.find('@'));
                std::string version = arg.substr(arg.find('@')+1);
                auto idx = load_index(indexFile);
                std::optional<IndexEntry> found;
                for (auto &ie : idx) if (ie.name==name && ie.version==version) { found = ie; break; }
                if (!found) {
                    // try to fetch from local cpm.toml sources
                    std::cout << "Package not found in registry; attempting fetch from configured sources...\n";
                    auto sources = parse_cpm_sources(std::filesystem::current_path() / "cpm.toml");
                    std::optional<std::filesystem::path> fetched;
                    if (!sources.empty()) {
                        fetched = fetch_from_sources(name, version, sources);
                    }
                    if (!fetched) {
                        std::cerr << "package not found in registry and fetch failed\n";
                        return 1;
                    }
                    // publish fetched archive into local registry
                    std::filesystem::create_directories(registry / "packages");
                    auto destArchive = registry / "packages" / fetched->filename();
                    std::filesystem::copy_file(*fetched, destArchive, std::filesystem::copy_options::overwrite_existing);
                    std::ifstream in(destArchive, std::ios::binary); std::ostringstream sss; sss << in.rdbuf(); auto sha = cppm::sha256_hex(sss.str());
                    auto now = std::chrono::system_clock::now(); std::time_t t = std::chrono::system_clock::to_time_t(now);
                    char buf[64];
#if defined(_MSC_VER)
    std::tm tm{}; gmtime_s(&tm, &t); std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
#else
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
#endif
                    IndexEntry e; e.name = name; e.version = version; e.path = (registry / "packages" / (name+"@"+version)).string(); e.archive = destArchive.string(); e.sha256 = sha; e.timestamp = buf; e.metadata = "";
                    auto idx = load_index(indexFile); idx.push_back(e); save_index(indexFile, idx);
                    found = e;
                }
                auto archivePath = std::filesystem::path(found->archive.empty() ? found->path : std::filesystem::path(found->archive));
                if (!std::filesystem::exists(archivePath)) { std::cerr << "archive missing: " << archivePath.string() << "\n"; return 1; }
                // verify sha256
                std::ifstream in(archivePath, std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); auto sha = cppm::sha256_hex(ss.str());
                if (!found->sha256.empty() && sha != found->sha256) { std::cerr << "sha256 mismatch\n"; return 1; }
                // extract to registry/installed/name@ver
                auto dest = registry / "installed" / (name + "@" + version);
                if (std::filesystem::exists(dest)) { std::cerr << "already installed\n"; return 1; }
                std::filesystem::create_directories(dest);
                std::string cmdline = "tar -C \"" + dest.string() + "\" -xzf \"" + archivePath.string() + "\"";
                std::cout << "Running: " << cmdline << "\n";
                if (std::system(cmdline.c_str()) != 0) { std::cerr << "extract failed\n"; return 1; }
                // register installed package in index (path points to installed dir)
                auto idx2 = load_index(indexFile);
                idx2.push_back(IndexEntry{ name, version, dest, found->archive, found->sha256, found->timestamp, found->metadata });
                save_index(indexFile, idx2);
                std::cout << "Installed " << name << "@" << version << " to " << dest.string() << "\n";
                return 0;
            } else {
                // local directory install (legacy) - prefer move (zero-copy)
                std::filesystem::path src = arg;
                if (!std::filesystem::exists(src) || !std::filesystem::is_directory(src)) { std::cerr << "package path must be an existing directory\n"; return 1; }
                auto meta = parse_toml_metadata(src / "cppmod.toml");
                std::filesystem::path srcAbs = std::filesystem::absolute(src);
                std::string name = srcAbs.filename().string(); std::string version = "0.0.0";
                if (meta) { name = meta->name; version = meta->version; }
                name = sanitize_name(name);
                if (name.empty()) name = std::string("pkg_") + sanitize_name(srcAbs.filename().string());
                auto dest = registry / (name + "@" + version);
                if (std::filesystem::exists(dest)) {
                    // Check registry index; if missing, register existing directory
                    auto idx = load_index(indexFile);
                    bool indexed = false;
                    for (auto &ie : idx) if (ie.name==name && ie.version==version) { indexed = true; break; }
                    if (indexed) { std::cerr << "package already installed: " << dest.string() << "\n"; return 1; }
                    // register existing destination
                    idx.push_back(IndexEntry{ name, version, dest, "", "", "", "" });
                    save_index(indexFile, idx);
                    std::cout << "Registered existing package: " << name << "@" << version << " -> " << dest.string() << "\n";
                    return 0;
                }
                try {
                    if (std::filesystem::exists(dest)) std::filesystem::remove_all(dest);
                    std::filesystem::rename(src, dest);
                } catch (const std::exception& ex) {
                    try {
                        std::filesystem::create_directories(dest);
                        std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                        std::filesystem::remove_all(src);
                    } catch (const std::exception& ex2) {
                        std::cerr << "Failed to move or copy package into registry: " << ex2.what() << "\n";
                        return 1;
                    }
                }
                auto idx = load_index(indexFile);
                idx.push_back(IndexEntry{ name, version, dest, "", "", "", "" });
                save_index(indexFile, idx);
                std::cout << "Installed " << name << "@" << version << " -> " << dest.string() << "\n";
                return 0;
            }
        }

        if (cmd == "remove") {
            if (argc < 3) { std::cerr << "remove requires name@version\n"; return 1; }
            std::string spec = argv[2];
            auto pos = spec.find('@');
            if (pos == std::string::npos) { std::cerr << "spec must be name@version\n"; return 1; }
            std::string name = spec.substr(0,pos); std::string ver = spec.substr(pos+1);
            auto idx = load_index(indexFile);
            std::vector<IndexEntry> remaining;
            bool found = false;
            for (auto &e : idx) {
                if (e.name==name && e.version==ver) {
                    if (std::filesystem::exists(e.path)) std::filesystem::remove_all(e.path);
                    found = true;
                } else remaining.push_back(e);
            }
            if (!found) { std::cerr << "package not found\n"; return 1; }
            save_index(indexFile, remaining);
            std::cout << "Removed " << spec << "\n";
            return 0;
        }

        if (cmd == "scan") {
            // reuse earlier behaviour: scan paths and print module graph
            std::vector<std::filesystem::path> roots;
            if (argc > 2) for (int i = 2; i < argc; ++i) roots.emplace_back(argv[i]);
            else roots.push_back(std::filesystem::current_path());

            DependencyGraph graph;
            const std::vector<std::string> exts = {".cpp", ".cc", ".cxx", ".c", ".ixx", ".cppm", ".h", ".hpp"};
            for (const auto& root : roots) {
                if (!std::filesystem::exists(root)) { std::cerr << "Path not found: " << root << "\n"; continue; }
                std::filesystem::recursive_directory_iterator it(root), end;
                for (; it != end; ++it) {
                    if (!it->is_regular_file()) continue;
                    auto p = it->path(); auto ext = p.extension().string();
                    bool ok = false; for (auto& e : exts) if (ext==e) { ok=true; break; } if (!ok) continue;
                    auto res = ModuleScanner::scanFile(p);
                    for (auto& err : res.errors) std::cerr << "[scan error] " << err << "\n";
                    for (auto& u : res.units) graph.addUnit(u);
                }
            }
            const auto& nodes = graph.nodes();
            for (const auto& [key, node] : nodes) {
                std::cout << key;
                if (!node.unit.filePath.empty()) std::cout << "  (" << node.unit.filePath.string() << ")";
                std::cout << "\n";
                for (const auto& dep : node.dependsOn) std::cout << "  -> " << dep << "\n";
            }
            return 0;
        }

        if (cmd == "autogen") {
            std::filesystem::path target = std::filesystem::current_path();
            if (argc >= 3) target = argv[2];
            bool ok = generate_cppmod_toml(target);
            if (ok) std::cout << "Generated cppmod.toml at " << (target / "cppmod.toml").string() << "\n";
            else std::cerr << "Failed to generate cppmod.toml (file may already exist)\n";
            return ok ? 0 : 1;
        }

        if (cmd == "test") {
            if (argc < 3) { std::cerr << "test requires package spec name@version\n"; return 1; }
            bool forceFallback = false;
            int specArg = 2;
            if (argc >= 3 && std::string(argv[2]) == "--force-fallback") { forceFallback = true; specArg = 3; }
            if (specArg >= argc) { std::cerr << "test requires package spec name@version\n"; return 1; }
            std::string spec = argv[specArg]; auto pos = spec.find('@'); if (pos==std::string::npos) { std::cerr<<"spec must be name@version\n"; return 1; }
            std::string name = spec.substr(0,pos); std::string ver = spec.substr(pos+1);
            auto idx = load_index(indexFile);
            std::optional<IndexEntry> found;
            // Prefer already-installed directory entries
            for (auto &e : idx) if (e.name==name && e.version==ver && std::filesystem::exists(e.path) && std::filesystem::is_directory(e.path)) { found = e; break; }
            // Otherwise prefer archive entries that exist
            if (!found) for (auto &e : idx) if (e.name==name && e.version==ver && !e.archive.empty() && std::filesystem::exists(std::filesystem::path(e.archive))) { found = e; break; }
            // Fallback to any matching entry
            if (!found) for (auto &e : idx) if (e.name==name && e.version==ver) { found = e; break; }
            if (!found) { std::cerr << "package not found in registry\n"; return 1; }
            auto pkgPath = found->path;
            if (!std::filesystem::exists(pkgPath)) { std::cerr << "package files missing: " << pkgPath.string() << "\n"; return 1; }

            // Create temp dir for staged build
            auto tmp = std::filesystem::temp_directory_path() / ("cpm_test_" + name + "_" + ver);
            std::filesystem::remove_all(tmp);
            std::filesystem::create_directories(tmp);

            auto cmakeLists = tmp / "CMakeLists.txt";
            std::ofstream cml(cmakeLists);
            cml << "cmake_minimum_required(VERSION 3.23)\n";
            cml << "project(cpm_test LANGUAGES CXX)\n";
            cml << "set(CMAKE_CXX_STANDARD 20)\n";

            // Collect package sources and create a library target so CMake will build module units
            std::vector<std::filesystem::path> pkgSources;
            for (auto &p : std::filesystem::recursive_directory_iterator(pkgPath)) {
                if (!p.is_regular_file()) continue;
                auto e = p.path().extension().string();
                if (e==".ixx" || e==".cppm" || e==".cpp" || e==".cc" || e==".c") pkgSources.push_back(p.path());
            }

            // Create test.cpp that imports discovered module interfaces (fallbacks to package name if none found)
            auto testcpp = tmp / "test.cpp";
            std::ofstream t(testcpp);
            // discover module names by scanning interface units and headers
            std::set<std::string> moduleNames;
            // accept identifiers that may include :: and dots (e.g. cxx20::modules::examples)
            std::regex export_rx("export\\s+module\\s+([A-Za-z0-9_:\\.] +)", std::regex_constants::icase);
            std::regex module_rx("\\bmodule\\s+([A-Za-z0-9_:\\.] +)", std::regex_constants::icase);
            for (auto &ps : pkgSources) {
                auto res = ModuleScanner::scanFile(ps);
                for (auto &u : res.units) {
                    if (!u.logicalName.empty()) moduleNames.insert(u.logicalName);
                }
                // fallback: also search source text for 'export module' or 'module' declarations
                try {
                    std::ifstream in(ps);
                    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    std::smatch m;
                    if (std::regex_search(content, m, export_rx) && m.size()>=2) moduleNames.insert(m[1].str());
                    if (std::regex_search(content, m, module_rx) && m.size()>=2) moduleNames.insert(m[1].str());
                } catch(...) {}
            }

            // also scan headers in include/ for module declarations
            if (moduleNames.empty()) {
                auto incdir = pkgPath / "include";
                if (std::filesystem::exists(incdir) && std::filesystem::is_directory(incdir)) {
                    for (auto &f : std::filesystem::recursive_directory_iterator(incdir)) {
                        if (!f.is_regular_file()) continue;
                        auto ext = f.path().extension().string();
                        if (ext==".h" || ext==".hpp" || ext==".hh" || ext==".ixx" || ext==".cppm") {
                            try {
                                std::ifstream in(f.path());
                                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                                std::smatch m;
                                if (std::regex_search(content, m, export_rx) && m.size()>=2) { moduleNames.insert(m[1].str()); continue; }
                                if (std::regex_search(content, m, module_rx) && m.size()>=2) { moduleNames.insert(m[1].str()); continue; }
                            } catch(...) {}
                        }
                    }
                }
            }

            // if still empty, look for files in modules/ directory and use filename stems
            if (moduleNames.empty()) {
                auto moddir = pkgPath / "modules";
                if (std::filesystem::exists(moddir) && std::filesystem::is_directory(moddir)) {
                    for (auto &f : std::filesystem::directory_iterator(moddir)) {
                        if (!f.is_regular_file()) continue;
                        auto ext = f.path().extension().string();
                        if (ext==".cppm" || ext==".ixx") {
                            auto stem = f.path().stem().string();
                            if (!stem.empty()) moduleNames.insert(stem);
                        }
                    }
                }
            }

            if (moduleNames.empty()) {
                // no module names found — attempt to include a header instead of importing the package name
                std::string pickedHeader;
                auto incdir = pkgPath / "include";
                if (std::filesystem::exists(incdir) && std::filesystem::is_directory(incdir)) {
                    // prefer header matching package name
                    std::vector<std::string> candidates = { name + ".hpp", name + ".h", name + ".hh" };
                    for (auto &c : candidates) {
                        auto p = incdir / c;
                        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) { pickedHeader = p.string(); break; }
                    }
                    // otherwise pick the first header found
                    if (pickedHeader.empty()) {
                        for (auto &f : std::filesystem::recursive_directory_iterator(incdir)) {
                            if (!f.is_regular_file()) continue;
                            auto ext = f.path().extension().string();
                            if (ext==".h" || ext==".hpp" || ext==".hh") { pickedHeader = f.path().string(); break; }
                        }
                    }
                }

                if (!pickedHeader.empty()) {
                    t << "#include \"" << pickedHeader << "\"\n";
                    t << "int main(){}\n";
                } else {
                    // final fallback: no modules or headers detected — produce minimal main to avoid assuming module name
                    t << "int main(){}\n";
                }
            } else {
                for (auto &m : moduleNames) t << "import " << m << ";\n";
                t << "int main(){}\n";
            }
            t.close();
            if (!pkgSources.empty()) {
                cml << "add_library(pkg_" << name << " STATIC\n";
                for (auto &ps : pkgSources) cml << "    \"" << ps.generic_string() << "\"\n";
                cml << ")\n";
                cml << "target_include_directories(pkg_" << name << " PUBLIC \"" << pkgPath.generic_string() << "\" \"" << (pkgPath / "include").generic_string() << "\")\n";
                cml << "add_executable(testprog test.cpp)\n";
                cml << "target_link_libraries(testprog PRIVATE pkg_" << name << ")\n";
            } else {
                cml << "add_executable(testprog test.cpp)\n";
                cml << "target_include_directories(testprog PRIVATE \"" << pkgPath.generic_string() << "\" \"" << (pkgPath / "include").generic_string() << "\")\n";
            }
            cml.close();

            // Try to run cmake configure + build (defers to user's build tool: Ninja, MSBuild, etc.)
            if (!forceFallback) {
                std::string cfg = "cmake -S \"" + tmp.string() + "\" -B \"" + (tmp / "build").string() + "\"";
                std::cout << "Running: " << cfg << "\n";
                int rc = std::system(cfg.c_str());
                if (rc == 0) {
                    std::string buildcmd = "cmake --build \"" + (tmp / "build").string() + "\" --config Debug";
                    std::cout << "Running: " << buildcmd << "\n";
                    rc = std::system(buildcmd.c_str());
                    if (rc == 0) { std::cout << "Test succeeded via CMake build\n"; return 0; }
                }
            }

            // Fallback: try direct compiler invocations (best-effort)
            std::vector<std::string> compilers = {"g++","clang++","icx","c++"};
            bool ok = false;
            for (auto &comp : compilers) {
                std::string modflag;
                // Detect whether the compiler accepts module-related flags before using them
                auto supports_flag = [&](const std::string &flag) -> bool {
                    try {
                        auto test_src = tmp / "cpm_flag_test.cpp";
                        std::ofstream ofs(test_src);
                        ofs << "int main(){}\n";
                        ofs.close();
                        auto test_obj = tmp / "cpm_flag_test.o";
                        std::string cmd = comp + " -std=c++20 " + flag + " -c \"" + test_src.string() + "\" -o \"" + test_obj.string() + "\"";
                        int rc = std::system(cmd.c_str());
                        std::error_code ec;
                        std::filesystem::remove(test_src, ec);
                        std::filesystem::remove(test_obj, ec);
                        return rc == 0;
                    } catch(...) { return false; }
                };

                if (comp.find("g++") != std::string::npos || comp.find("clang++") != std::string::npos) {
                    if (supports_flag("-fmodules-ts")) {
                        modflag = " -fmodules-ts -fmodules-cache-path=\"gcm.cache\"";
                    } else {
                        // compiler does not accept -fmodules-ts; avoid passing unknown argument
                        modflag = "";
                    }
                } else modflag = "";
                auto gcm_cache = tmp / "gcm.cache";
                std::filesystem::create_directories(gcm_cache);

                // compile module/interface files found in package
                std::vector<std::filesystem::path> modSrcs;
                for (auto &p : std::filesystem::recursive_directory_iterator(pkgPath)) {
                                    if (!p.is_regular_file()) continue;
                                    auto e = p.path().extension().string();
                                    if (e==".ixx" || e==".cppm" || e==".cpp" || e==".cc" ) {
                                        auto res = ModuleScanner::scanFile(p.path());
                                        if (!res.units.empty()) modSrcs.push_back(p.path());
                                    }
                }

                auto oldcwd = std::filesystem::current_path();
                std::filesystem::current_path(tmp);

                bool stageFail = false;
                std::vector<std::string> objs;
                for (auto &s : modSrcs) {
                                    auto outobj = tmp / (s.filename().string() + ".o");
                                    std::string cmdline = comp + " -std=c++20" + modflag + " -c \"" + s.string() + "\" -I\"" + pkgPath.string() + "\" -I\"" + (std::filesystem::path(pkgPath) / "include").string() + "\" -o \"" + outobj.string() + "\" ";
                                    std::cout << "Running: " << cmdline << "\n";
                                    int r = std::system(cmdline.c_str());
                                    if (r != 0) { stageFail = true; break; }
                                    objs.push_back(outobj.string());
                }

                if (!stageFail) {
                                    std::string linkcmd = comp + " -std=c++20" + modflag + " \"" + testcpp.string() + "\" -I\"" + pkgPath.string() + "\" -I\"" + (std::filesystem::path(pkgPath) / "include").string() + "\" ";
                                    for (auto &o : objs) linkcmd += " \"" + o + "\" ";
                                    linkcmd += " -o \"" + (tmp / "testprog").string() + "\"";
                                    std::cout << "Running: " << linkcmd << "\n";
                                    int r2 = std::system(linkcmd.c_str());
                                    if (r2 == 0) { ok = true; std::cout << "Test succeeded with compiler: " << comp << "\n"; }
                }

                std::filesystem::current_path(oldcwd);
                if (ok) break;
            }

            if (!ok) { std::cerr << "Test failed with all strategies\n"; return 1; }
            return 0;
        }

        std::cerr << "Unknown command: " << cmd << "\n";
        return 1;

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }
}
