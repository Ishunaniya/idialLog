#!/usr/bin/env python3
"""gen_all_prints 的反循环单测：钉死构建范围、调用点、格式和通道包络。"""
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sim.gen_all_prints import (
    extract_text,
    generate,
    in_scope,
    product_of,
    variants,
    write_manifest,
)


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


V2_SOURCE = r'''
void f(FILE *fp) {
    log_err("err=%d", n);
    log_warning("warning=%s", why);
    log_notice("notice");
    log_info("first\nsecond=%d", n);
    log_debug("debug=%x", n);
    syslog(LOG_USER | LOG_ERR, "direct=%s", why);
    logger(LOG_WARNING, "wrapped=%d", n);
    logger("default logger");
    printf("raw %s\n", value);
    fprintf(stderr, "stderr=%d\n", n);
    fprintf(fp, "file sink must not count\n");
    system("ip -br addr show usb0 2>/dev/null | logger -t modem_mng_v2; "
           "ip -br addr show usb0 1>&2");
    system("ip route show default 2>/dev/null | "
           "logger -p user.err -t modem_mng_v2");
    system("ip link set usb0 up");
    ENABLE_AT_DEBUG(1);
    if (IS_ENABLE_AT_DEBUG()) md_proc_error(ctx);
    openlog("modem_mng_v2", LOG_PID, LOG_USER);
}
'''


V2_BUILD_SOURCES = {
    "main.c",
    "src/gpio/gpio.c",
    "src/modem/modem.c",
    "src/serial/serial.c",
    "src/modem/at_op.c",
    "src/modem/at_func.c",
    "src/at_server/at_server.c",
    "src/log/log.c",
    "src/rb/rb.c",
    "src/config/config.c",
}


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
    assert product_of("/workspace/modem_mng_v2") == "modem_mng_v2"
    assert variants("modem_mng_v2", "src/modem/modem.c") == ["modem_mng_v2"]
    assert {path for path in V2_BUILD_SOURCES
            if in_scope("modem_mng_v2", path)} == V2_BUILD_SOURCES
    for path in ("src/log/log.h", "scripts/atcmd.py", "src/not_built/extra.c"):
        assert not in_scope("modem_mng_v2", path), path

    v2_calls = extract_text("modem_mng_v2", "branch", "src/modem/modem.c", V2_SOURCE)
    got_v2 = {(c.api, c.channel, c.severity, c.fmt) for c in v2_calls}
    want_v2 = {
        ("log_err", "syslog", "err", "err=%d"),
        ("log_warning", "syslog", "warning", "warning=%s"),
        ("log_notice", "syslog", "notice", "notice"),
        ("log_info", "syslog", "info", "first\nsecond=%d"),
        ("log_debug", "syslog", "debug", "debug=%x"),
        ("syslog", "syslog", "err", "direct=%s"),
        ("logger", "syslog", "warning", "wrapped=%d"),
        ("logger", "syslog", "notice", "default logger"),
        ("printf", "console", "", "raw %s\n"),
        ("fprintf", "console", "", "stderr=%d\n"),
        ("system/logger", "syslog", "notice",
         "<dynamic:logger-output:ip -br addr show usb0 2>/dev/null>"),
        ("system/logger", "syslog", "err",
         "<dynamic:logger-output:ip route show default 2>/dev/null>"),
    }
    assert got_v2 == want_v2, f"\nmissing={want_v2-got_v2}\nextra={got_v2-want_v2}"

    with tempfile.TemporaryDirectory() as tmp:
        fixture = Path(tmp) / "v2.log"
        manifest = Path(tmp) / "v2.manifest.tsv"
        line_count, counts = generate(v2_calls, fixture)
        write_manifest(manifest, v2_calls)
        lines = fixture.read_text(encoding="utf-8").splitlines()
        header = manifest.read_text(encoding="utf-8").splitlines()[0].split("\t")

    assert line_count == len(lines) == len(want_v2)
    assert counts == {"console": 2, "syslog": 10}
    assert header[:8] == [
        "product", "channel", "api", "dynamic", "ref", "path", "line", "format"]
    assert header[8] == "severity"
    assert "raw VALUE" in lines and "stderr=1" in lines
    assert any(re.fullmatch(
        r"Aug 24 \d{2}:\d{2}:\d{2} device user\.err "
        r"modem_mng_v2\[1000\]: err=1", line) for line in lines)
    assert any("device user.warn modem_mng_v2[1000]: warning=VALUE" in line
               for line in lines)
    assert any("device user.notice modem_mng_v2[1000]: notice" in line
               for line in lines)
    assert any("device user.info modem_mng_v2[1000]: first second=1" in line
               for line in lines)
    assert any("device user.debug modem_mng_v2[1000]: debug=1" in line
               for line in lines)
    assert any("device user.notice modem_mng_v2[1000]: "
               "DYNAMIC LOGGER OUTPUT (ip -br addr show usb0 2>/dev/null)" in line
               for line in lines)
    assert not any("[ERR]" in line or line == "second=1" for line in lines)

    print(f"sourceaudit: 旧产品 {len(got)} 种、v2 {len(got_v2)} 种调用全部按预期提取")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
