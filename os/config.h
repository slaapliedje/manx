/*
 * config.h - the settings file, $UB_HOME/config (else ~/.ub/config):
 * "key = value" lines, # comments. An environment variable
 * UB_<KEY> (upper case) overrides a key; missing keys take the caller's
 * default.
 */
#ifndef UB_CONFIG_H
#define UB_CONFIG_H

/* Read the file (a missing file is fine). 0, or -1 with the problem in
 * *err (a static message: the line that couldn't be understood). */
int config_load(const char *path, const char **err);

const char *config_str(const char *key, const char *def);
int config_bool(const char *key, int def);	/* on/off, yes/no, 1/0 */
long config_long(const char *key, long def);

#endif /* UB_CONFIG_H */
