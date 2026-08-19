#!/usr/bin/env python3
"""gen_all_prints 的反循环单测：直接给源码，钉死提取调用点和完整格式。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sim.gen_all_prints import extract_text, variants


SOURCE = r'''
// dial_log("[COMMENT] must not count\n");
/* printf("block comment"); */
#if 0
SEAS_LOG_INFO("dead");
#else
SEAS_LOG_ERROR(
    "[LIVE] first "
    "second=%d");
#endif
#if 1
printf("constant true\n");
#else
printf("constant false must not count\n");
#endif
void dial_log(const char *fmt, ...) { vprintf(fmt, ap); }
void f(FILE *fp) {
    dial_log("[SD] " "joined=%s\n", value);
    printf("raw %d\n", n);
    fprintf(stderr, "stderr: %s\n", why);
    fprintf(fp, "state file, not output\n");
    ALOGE("android=%d", n);
    QLOGI(NW_LOG_TAG, "qlog=%s", value);
    std::cout << "stream=" << value << "!" << std::endl;
}
'''


def main() -> int:
    calls = extract_text("fixture", "branch", "fixture.cpp", SOURCE)
    got = {(c.api, c.channel, c.fmt) for c in calls}
    want = {
        ("SEAS_LOG_ERROR", "seas", "[LIVE] first second=%d"),
        ("vprintf", "console", "<dynamic:fmt>"),
        ("dial_log", "sd", "[SD] joined=%s\n"),
        ("printf", "console", "raw %d\n"),
        ("printf", "console", "constant true\n"),
        ("fprintf", "console", "stderr: %s\n"),
        ("ALOGE", "android", "android=%d"),
        ("QLOGI", "android", "qlog=%s"),
        ("std::cout", "console", "stream=%s!\n"),
    }
    assert got == want, f"\nmissing={want-got}\nextra={got-want}"
    assert variants("modem_mng", "ec200a/dial/dial.cpp") == ["modem_ec200a_ag35"]
    assert variants("modem_mng", "eg25/dial/dial.c") == ["modem_eg25"]
    assert variants("modem_mng", "logger_sd.c") == [
        "modem_ec200a_ag35", "modem_eg25"]
    print(f"sourceaudit: {len(got)} 种调用全部按预期提取")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
