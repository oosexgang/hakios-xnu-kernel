# Commands run by the launchd job com.hakios.demo (see com.hakios.demo.plist).
# Control Center pulled down from the top right corner and closed (home button), three times, three seconds open
# and three closed: for a screen recording (rec_secs=N rec_start=S) and glframes=1, to see in which frame each of
# its parts appears.
/redwood/bin/sound_test springboard
/redwood/bin/rwtouch wait 15
/redwood/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/redwood/bin/rwtouch button home wait 3
/redwood/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/redwood/bin/rwtouch button home wait 3
/redwood/bin/rwtouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/redwood/bin/rwtouch button home wait 3
