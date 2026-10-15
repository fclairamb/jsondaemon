#ifndef JD_LOG_H
#define JD_LOG_H

enum jd_log_level {
	JD_LOG_ERROR,
	JD_LOG_WARN,
	JD_LOG_INFO,
	JD_LOG_DEBUG,
};

extern int jd_log_level;

void jd_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_error(...) jd_log(JD_LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  jd_log(JD_LOG_WARN, __VA_ARGS__)
#define log_info(...)  jd_log(JD_LOG_INFO, __VA_ARGS__)
#define log_debug(...) jd_log(JD_LOG_DEBUG, __VA_ARGS__)

#endif /* JD_LOG_H */
