#include "res/vfs.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>

#include "util/text.h"

namespace ore {
namespace {

uint32_t rd32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

bool read_host_file(const std::string &path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseeko(f, 0, SEEK_END);
	off_t n = ftello(f);
	fseeko(f, 0, SEEK_SET);
	out.resize(size_t(n));
	bool ok = n == 0 || fread(out.data(), 1, size_t(n), f) == size_t(n);
	fclose(f);
	return ok;
}

// Finds "<dir>/<name>" ignoring case of `name`.
std::string find_ci(const std::string &dir, const std::string &name) {
	DIR *d = opendir(dir.c_str());
	if (!d) return std::string();
	std::string want = to_upper_ascii(name), found;
	while (dirent *e = readdir(d)) {
		if (to_upper_ascii(e->d_name) == want) {
			found = dir + "/" + e->d_name;
			break;
		}
	}
	closedir(d);
	return found;
}

}  // namespace

std::map<std::string, LpkKey> parse_gameinit_keys(const std::vector<uint8_t> &sob) {
	std::map<std::string, LpkKey> keys;
	if (sob.size() < 16 || memcmp(sob.data(), "SOB0", 4) != 0) return keys;
	size_t off = rd32(&sob[4]) + 8;
	if (off + 8 >= sob.size()) return keys;
	uint32_t index_len = rd32(&sob[off]);
	size_t base = off + 8;
	size_t nwords = (sob.size() - base) / 4;
	auto w = [&](size_t i) { return rd32(&sob[base + i * 4]); };
	size_t last = index_len / 4 >= 28 ? index_len / 4 - 28 : 0;
	if (last + 29 > nwords) last = nwords > 29 ? nwords - 29 : 0;
	for (size_t i = 0; i < last;) {
		if (w(i) == 0x28 && w(i + 1) == 0 && w(i + 2) != 0 && w(i + 3) == 8 && w(i + 4) == 1 &&
		    w(i + 5) == 8 && w(i + 6) == 1 && w(i + 7) == 8 && w(i + 8) != 0 && w(i + 9) == 8 &&
		    w(i + 10) != 0 && w(i + 11) == 5 && w(i + 17) == 0x28 && w(i + 18) == 0 &&
		    w(i + 19) != 0 && w(i + 20) == 8 && w(i + 21) != 0 && w(i + 22) == 8 &&
		    w(i + 23) == 0xFFFFFFFF && w(i + 24) == 8 && w(i + 25) == 1 && w(i + 26) == 8 &&
		    w(i + 27) == 1 && w(i + 28) == 5) {
			size_t name_at = base + i * 4 + w(i + 21) - w(i + 2) + 0x34;
			if (name_at < sob.size()) {
				const char *s = reinterpret_cast<const char *>(&sob[name_at]);
				size_t len = strnlen(s, sob.size() - name_at);
				LpkKey k;
				k.key1 = w(i + 8);
				k.key2 = w(i + 10);
				keys[to_upper_ascii(std::string(s, len))] = k;
			}
			i += 0x22;
		} else {
			++i;
		}
	}
	return keys;
}

bool Vfs::mount_archives(const std::string &data_dir, std::string *error) {
	std::string script_path = find_ci(data_dir, "SCRIPT.LPK");
	if (script_path.empty()) {
		if (error) *error = "SCRIPT.LPK not found in " + data_dir;
		return false;
	}
	auto script = LpkArchive::open(script_path, "SCRIPT", nullptr, error);
	if (!script) return false;
	std::vector<uint8_t> gameinit;
	if (!script->read("GAMEINIT.SOB", gameinit)) {
		if (error) *error = "gameinit.sob missing from SCRIPT.LPK";
		return false;
	}
	archives_["SCRIPT"] = std::move(script);
	for (auto &kv : parse_gameinit_keys(gameinit)) {
		std::string base = kv.first.substr(0, kv.first.rfind('.'));
		std::string path = find_ci(data_dir, kv.first);
		if (path.empty()) continue;  // optional archive absent
		auto arc = LpkArchive::open(path, base, &kv.second, error);
		if (!arc) return false;
		archives_[base] = std::move(arc);
	}
	return true;
}

void Vfs::add_overlay(const std::string &dir, const std::string &virtual_prefix) {
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	std::string prefix = normalize_path(virtual_prefix);
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] == '.') continue;
		std::string host = dir + "/" + e->d_name;
		struct stat st;
		if (stat(host.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
		loose_[prefix + "\\" + to_upper_ascii(e->d_name)] = host;
	}
	closedir(d);
}

bool Vfs::split(const std::string &path, std::string &dir, std::string &name) const {
	std::string p = normalize_path(path);
	size_t slash = p.find('\\');
	if (slash == std::string::npos) return false;
	dir = p.substr(0, slash);
	name = p.substr(slash + 1);
	return true;
}

bool Vfs::locate(const std::string &path, Location &loc) const {
	std::string p = normalize_path(path);
	auto it = loose_.find(p);
	if (it != loose_.end()) {
		loc.host_path = it->second;
		loc.archive = nullptr;
		loc.entry.clear();
		return true;
	}
	std::string dir, name;
	if (!split(p, dir, name)) return false;
	auto a = archives_.find(dir);
	if (a == archives_.end() || !a->second->contains(name)) return false;
	loc.host_path.clear();
	loc.archive = a->second.get();
	loc.entry = name;
	return true;
}

bool Vfs::exists(const std::string &path) const {
	Location loc;
	return locate(path, loc);
}

bool Vfs::read(const std::string &path, std::vector<uint8_t> &out) const {
	Location loc;
	if (!locate(path, loc)) return false;
	if (!loc.archive) return read_host_file(loc.host_path, out);
	return loc.archive->read(loc.entry, out);
}

const LpkArchive *Vfs::archive(const std::string &upper_name) const {
	auto a = archives_.find(upper_name);
	return a == archives_.end() ? nullptr : a->second.get();
}

}  // namespace ore
