#!/usr/bin/env python3
"""
mkwallpaper.py SYSROOT: iOS 16's default wallpaper (7400, WWDC 2022) with its head masked by a layer's mask.

The scene draws "Head.HEIC" over "Head Mask.HEIC", an image of pure red with a soft alpha, through a compositing filter
(sourceAtop): the red is there only for its alpha and on an iPhone none of it is seen. Here the head does not get drawn
through that filter, on either Metal path, and what stands at the top of the screen is the mask itself: the sampled
screen is the wallpaper's blue mixed with pure red by that alpha (the phone, 4 November). The layer's own mask does the
same thing, and CoreAnimation's masks are drawn right here. Why the filter fails is not known.

SYSROOT is hard links (or clones) of sysimage/rootfs: a scene is written as a new file in place of the link, never
into it, so the extracted system stays as it came out of the IPSW.
"""
import glob
import os
import re
import sys

MASK = re.compile(
    r'(<CALayer\b[^>]*\bbounds="(?P<bounds>0 0 (?P<w>[\d.]+) (?P<h>[\d.]+))"[^>]*\bname="Head Mask\.HEIC"[^>]*>\s*)'
    r'<contents type="CGImage" src="(?P<src>assets/Head%20Mask\.HEIC)"/>')
FILTER = re.compile(r'\s*<compositingFilter type="CAFilter" filter="sourceAtop" name="sourceAtop"/>')


def rewrite(text):
    def mask(m):
        return ('%s<mask type="CALayer" allowsEdgeAntialiasing="1" bounds="%s" contentsFormat="RGBA8" position="%g %g">'
                '<contents type="CGImage" src="%s"/></mask>'
                % (m.group(1), m.group('bounds'), float(m.group('w')) / 2, float(m.group('h')) / 2, m.group('src')))
    text, masks = MASK.subn(mask, text)
    text, filters = FILTER.subn('', text)
    if masks != 1 or filters != 1:
        raise SystemExit('mkwallpaper.py: %d masks and %d filters, not one of each' % (masks, filters))
    return text


def main():
    pattern = 'Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/*/7400.WWDC_2022_Background-*.ca/main.caml'
    scenes = glob.glob(os.path.join(sys.argv[1], pattern))
    if not scenes:
        raise SystemExit('mkwallpaper.py: no scene of the wallpaper in ' + sys.argv[1])
    for scene in scenes:
        with open(scene) as f:
            text = rewrite(f.read())
        os.unlink(scene)
        with open(scene, 'w') as f:
            f.write(text)
    print('mkwallpaper.py: the head of the wallpaper masked by a layer in %d scenes' % len(scenes))


if __name__ == '__main__':
    main()
