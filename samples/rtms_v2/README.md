# modem_mng_v2 合成样本

`modem_mng_v2_synthetic.log` 是【合成，非真机】测试夹具，不能替代设备实测日志。

除首行的合成声明外，日志正文逐字取自
`rtms_sdk/apps/modem_mng_v2/src/modem/modem.c` 的 `log_info` / `log_err` 格式串；
时间、主机名、PID 及格式参数值是为覆盖 BusyBox RFC3164、模组识别、CSQ 指标和 WAN
断网配对而填入的测试值。syslog 包络采用目标 RK3576 Buildroot 中 BusyBox syslogd 的
`Mon DD HH:MM:SS host facility.priority app[pid]: message` 形态。

