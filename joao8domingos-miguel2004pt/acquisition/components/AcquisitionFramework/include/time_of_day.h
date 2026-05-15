
#ifndef TIME_READER_H
#define TIME_READER_H

#include <sys/time.h>
#include <cstdint>

class TimeReader {
public:
    
    static int64_t getCurrentTimeUs() {
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        return (int64_t)tv_now.tv_sec * 1000000LL + (int64_t)tv_now.tv_usec;
    }

    /**
     * @brief Retorna o tempo atual em milissegundos.
     */
    static int64_t getCurrentTimeMs() {
        return getCurrentTimeUs() / 1000;
    }
};

#endif