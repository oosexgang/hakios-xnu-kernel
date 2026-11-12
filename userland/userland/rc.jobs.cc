# Commands run by the launchd job com.hakios.demo (see com.hakios.demo.plist).
# Control Center pulled down from the top right corner and closed (home button), three times, three seconds open
# and three closed: for a screen recording (rec_secs=N rec_start=S) and glframes=1, to see in which frame each of
# its parts appears.
/hakios/bin/sound_test springboard
/hakios/bin/rwtouch wait 15
/hakios/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/hakios/bin/rwtouch button home wait 3
/hakios/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/hakios/bin/rwtouch button home wait 3
/hakios/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/hakios/bin/rwtouch button home wait 3
