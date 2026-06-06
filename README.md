# bsides-leeds-2026-badge
The Artie the Owl badge for BSIDES Leeds 2026!

Full details coming after the con! For now, take a look at the firmware for clues.

If you fork this repo and modify the firmware, we'll flash it to your badge on the day! Never done Arduino / Cpp code before? That's what ChatGPT is for...

We've included the firmware in firmware.ino, and also the compiled firmware in the compiled folder. Some of this firmware is pretty and some is ugly. 

This is the first bit of firmware where we've used AI to try and reduce flash footprint. It's always a battle with these badges to keep the flash footprint down, and in this case we have 8192KB of space available. As with all AI work, it's hard to stop it going off-piste. Some of the random functions are a bit heavy handed, but it was worth it to reduce the footprint and get all 6 games on the badge.

We've also included the BOM.csv, which tells you the parts used to build the badge. This may help you (or the AI) understand what you've got to work with. Mainly the LEDs (individually addressible) and microcontroller (ATTINY814).

This badge stands on the back of the fantastic work done by [MegaTinyCore](https://github.com/SpenceKonde/megaTinyCore). Without this, this firmware would be harder to read and the PTC library has been amazing to use.

