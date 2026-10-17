// Virtual file system: loose-file overlays over the game's LPK archives.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "res/lpk.h"

namespace ore {

class Vfs {
public:
	// data_dir holds the *.LPK archives (the installed game directory).
	bool mount_archives(const std::string &data_dir, std::string *error);

	// Adds a loose-file overlay: files under `dir` are visible as
	// `virtual_prefix\NAME` (e.g. dir=".../SCRIPT_CHS", prefix="SCRIPT").
	// Later overlays take precedence over earlier ones and over archives.
	void add_overlay(const std::string &dir, const std::string &virtual_prefix);

	// Path forms accepted: "PIC\\EF05A.ELG", "pic/ef05a.elg". Case-insensitive.
	bool read(const std::string &path, std::vector<uint8_t> &out) const;
	bool exists(const std::string &path) const;

	// Where a file physically lives, for streaming large media.
	struct Location {
		std::string host_path;      // loose file, or the LPK file
		const LpkArchive *archive;  // null for loose files
		std::string entry;          // archive entry name
	};
	bool locate(const std::string &path, Location &loc) const;

	const LpkArchive *archive(const std::string &upper_name) const;

private:
	bool split(const std::string &path, std::string &dir, std::string &name) const;

	std::map<std::string, std::unique_ptr<LpkArchive>> archives_;  // "SYS" -> archive
	// virtual upper-case path -> host path, filled by add_overlay
	std::map<std::string, std::string> loose_;
};

// Parses the per-archive key table embedded in gameinit.sob.
std::map<std::string, LpkKey> parse_gameinit_keys(const std::vector<uint8_t> &sob);

}  // namespace ore
