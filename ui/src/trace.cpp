#include "trace.h"
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace banana
{
	static FILE* traceFile = nullptr;

	void traceInit(const char* path)
	{
		if (traceFile) { fclose(traceFile); traceFile = nullptr; }
		if (!path) return;
		traceFile = fopen(path, "w");
		if (traceFile) setvbuf(traceFile, nullptr, _IONBF, 0);
	}

	void tracef(const char* fmt, ...)
	{
		if (!traceFile) return;
		va_list ap;
		va_start(ap, fmt);
		vfprintf(traceFile, fmt, ap);
		fputc('\n', traceFile);
		va_end(ap);
		fflush(traceFile);
		fsync(fileno(traceFile));	// survives a reset
	}
}
