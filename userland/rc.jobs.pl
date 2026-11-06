# Commands run by the launchd job com.redwood.demo (see com.redwood.demo.plist).
# preferenceloader, the package nearly every tweak with a settings pane depends on
# (RC_JOBS=rc.jobs.pl): it wants "mobilesubstrate", which ElleKit Provides, so apt can take it at
# all now; then Settings is opened, and the tweak loader says whether PreferenceLoader went into it.
/redwood/bin/sound_test springboard
/var/jb/bin/sh /var/jb/prep.sh
/redwood/bin/net_test waitaddr en0 60 resolve apt.procurs.us
/var/jb/usr/bin/apt-get update
/var/jb/usr/bin/apt-get install -y preferenceloader
/var/jb/usr/bin/dpkg-query -W -f${binary:Package}_${Version}_${Status}\n preferenceloader ellekit
/var/jb/bin/ls -la /var/jb/usr/lib/TweakInject/
/var/jb/usr/bin/uiopen prefs:
/redwood/bin/rwtouch wait 20
