#include <inc/lib.h>

int hwclock_time(int *t) {
        int sec, min, hour, day, mon, year;
        int time;
        const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int i, j;

        year = sys_get_hw_time(&sec, &min, &hour, &day, &mon);

        time = bcd2dec(hour) * 3600 + bcd2dec(min) * 60 + bcd2dec(sec);

        year = bcd2dec(year);

        // 1970 - 2069
        if (year < 70) {
                year = 2000 + year;
        }
        else {
                year = 1900 + year;
        }

        mon = bcd2dec(mon);
        day = bcd2dec(day);

        j = day;
        for (i = 0; i < mon - 1; i++) {
                j += d[i];
        }

        for (i = 1970; i < year; i++) {
                j += 365;
                if (j % 4 == 0) {
                        j++;
                }
        }

        time += j * 86400;

        if (t != NULL) {
                *t = time;
        }

        return time;
}

int bcd2dec(int bcd) {
        int i, r;

        r = 0;

        for (i = (2*sizeof(int)) - 1; i >= 0; i--) {
                r = (r * 10) + ((bcd >> (4 * i)) & 0xF);
        }

        return r;
}

