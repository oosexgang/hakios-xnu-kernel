# Sensors on the Infinix (X6826B): what the device tree says

Run 276 (27 October 2026), mtkloader's report of the tree LK hands over (mtkloader/mtkconnsys.h,
nodes whose name or compatible says sensor/accel/gsensor/gyro/alsps/magnet/scp):

- No accelerometer, gyroscope, light/proximity or magnetometer node at all: no `gsensor`, `alsps`,
  `gyro` or `msensor` on an I2C bus, as older MediaTek phones had them.
- `scp@10500000`, compatible `mediatek,scp`: reg 0x10500000 (512 KB SRAM), 0x105c0000 (0x3000),
  0x105c4000 (0x1000), 0x105d4000 (0x6000); interrupt SPI 0xCD.
- `reserved-memory/reserve-memory-scp_share`: 13 MB (0xd00000) anywhere in 0x40000000-0x90000000.
- `scp_dvfs`, `scp_infra@10001000`, and the sound's `snd_scp_ultra`, `snd_scp_spk`.
- The two `ccu_sensor_i2c_*` nodes are the camera's (CCU), not motion sensors; `thermal-sensor1/2` are
  board thermistors.

So the sensors are behind the SCP (MediaTek's "tinysys" Cortex-M co-processor running the sensor
hub): Android's kernel does not drive the chips, it talks to the SCP's firmware through IPI
(shared memory and the SCP's mailbox interrupt), and the firmware owns the I2C/SPI to the chips.

What a driver here would need, in order:
1. Whether the SCP is already running when XNU starts (LK loads the `scp` partition's image on
   MediaTek phones): read its status registers, read-only, the way connsys step 1 does.
2. Its firmware's IPI message format for the sensor hub (MediaTek's `sensorhub`/`scp` drivers in the
   vendor kernel sources: `drivers/misc/mediatek/sensors-1.0/sensorHub`, `scp/` -- the same
   MiCode/MTK kernel trees as CONNSYS).
3. Enabling the accelerometer and reading its events into IOHID for iOS (orientation, shake).

## Working (28 October 2026): the accelerometer read from XNU

`bsd/dev/redwood_scp.c`, boot-arg `scp_step=6` (each step includes the ones before it):

| Step | What | Run |
| --- | --- | --- |
| 1 | read: the SCP out of reset, not running; LK's firmware in its TCM, its DRAM at 0x9f900000 | 277, 278 |
| 2 | 1 to cfg+0, as Android's kernel does: it calibrates its oscillator ~5 s, then says it is there | 279-283 |
| 3 | its messages at TCM 0x5c0: IPI_SCP_A_READY (512 KB), IPI_LOGGER_INIT_A, IPI_SENSOR SCP_INIT_DONE | 286 |
| 4 | requests to the hub (awake through INFRA_IRQ_SET, send buffer at 0x6e0, GIPC_IN): answered | 287 |
| 5 | nanohub over IPI_CHRE: a ConfigCmd enabling the accelerometer, answered "success" -- then the SCP's watchdog: no DRAM to write into | 288 |
| 6 | first the DRAM FIFO (1 MB at 0x4e000000, header rp/wp/size, SENSOR_HUB_SET_CONFIG), then the config: 50 records a second, DIRECT_PUSH notifications | 291 |

Run 291, the phone lying face up: x -76, y -38, z 9868, then x 38, y 0, z 9812 -- in 1/1000 m/s^2,
gravity on z. Records are struct data_unit_t, 44 bytes: x, y, z at bytes 12, 16, 20.

Not done yet: handing the samples to iOS (an accelerometer service for backboardd/CoreMotion, so
that the screen rotates and shaking works), the other sensors (light, proximity: their nanohub types),
and doing all this in a thread instead of at boot.
