/*
 * file:        config.cc
 * description: quick and dirty config file parser
 *              env var overrides modeled on github.com/spf13/viper
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <stdlib.h>
#include <ctype.h>
#include <uuid/uuid.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
namespace fs = std::filesystem;

#include "config.h"

std::vector<std::string> cfg_path(
    {"lsvd.conf", "/usr/local/etc/lsvd.conf"});

static void split(std::string s, std::vector<std::string> &words) {
    std::string w = "";
    for (auto c : s) {
	if (!isspace(c))
	    w = w + c;
	else {
	    words.push_back(w);
	    w = "";
	}
    }
    if (w.size() > 0)
	words.push_back(w);
}

static long parseint(const char *_s)
{
    char *s = (char*)_s;
    long val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024*1024*1024);
    if (toupper(*s) == 'M')
        val *= (1024*1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

static long parseint(std::string &s)
{
    return parseint(s.c_str());
}

static std::map<std::string,cfg_backend> m = {{"file", BACKEND_FILE},
					      {"rados", BACKEND_RADOS}};


int lsvd_config::read() {
    for (auto f : cfg_path) {
	std::ifstream fp(f);
	if (!fp.is_open())
	    continue;
	std::string line;
	while (getline(fp, line)) {
	    if (line[0] == '#')
		continue;
	    std::vector<std::string> words;
	    split(line, words);
	    if (words[0] == "batch_size")
		batch_size = parseint(words[1]);
	    if (words[0] == "wcache_window")
		wcache_window = atoi(words[1].c_str());
	    if (words[0] == "cache_dir")
		cache_dir = words[1];
	    if (words[0] == "wcache_threads")
		wcache_threads = atoi(words[1].c_str());
	    if (words[0] == "xlate_threads")
		xlate_threads = atoi(words[1].c_str());
	    if (words[0] == "backend")
		backend = m[words[1]];
	    if (words[0] == "cache_size")
		cache_size = parseint(words[1]);
	}
	fp.close();
	break;
    }
    const char *val = NULL;
    if ((val = getenv("LSVD_BATCH_SIZE")))
	batch_size = parseint(val);
    if ((val = getenv("LSVD_CACHE_DIR")))
	cache_dir = std::string(val);
    if ((val = getenv("LSVD_WCACHE_WINDOW")))
	wcache_window = atoi(val);
    if ((val = getenv("LSVD_CACHE_DIR")))
	cache_dir = std::string(val);
    if ((val = getenv("LSVD_WCACHE_THREADS")))
	wcache_threads = atoi(val);
    if ((val = getenv("LSVD_XLATE_THREADS")))
	xlate_threads = atoi(val);
    if ((val = getenv("LSVD_BACKEND"))) {
	std::string word(val);
	backend = m[word];
    }
    if ((val = getenv("LSVD_CACHE_SIZE"))) 
	cache_size = parseint(val);

    return 0;			// success
}

std::string lsvd_config::cache_filename(uuid_t &uuid, const char *name) {
    char buf[256]; // PATH_MAX
    std::string file(name);
    if (backend == BACKEND_FILE) 
	file = fs::path(file).filename();
    
    sprintf(buf, "%s/%s.cache", cache_dir.c_str(), file.c_str());
    if (access(buf, R_OK|W_OK) == 0)
	return std::string((const char*)buf);

    char uuid_s[64];
    uuid_unparse(uuid, uuid_s);
    sprintf(buf, "%s/%s.cache", cache_dir.c_str(), uuid_s);
    return std::string((const char*)buf);
}

int main(int argc, char **argv) {
    auto cfg = new lsvd_config;
    cfg->read();

    printf("batch: %d\n", cfg->batch_size);
    printf("wc window %d\n", cfg->wcache_window);
    printf("cache: %s\n", cfg->cache_dir.c_str());
    printf("wc threads %d\n", cfg->wcache_threads);
    printf("xl threads %d\n", cfg->xlate_threads);
    printf("backend: %d\n", (int)cfg->backend);

    uuid_t uu;
    std::cout << cfg->cache_filename(uu, "foobar") << "\n";
}
