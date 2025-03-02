# Olimex's Linux Kernel Fork for Trexom Cosmo
This repository is a fork from Olimex's Linux kernel repository, and contains patches to adapt it on Trexom's Cosmo.
This repo may not be updated to latest LTS mainline version since we can only rely on latest Olimex's kernel branches, otherwise SoM's warranty will be broken.


# Structure
Olimex doesn't use tags to mark their version and create a new branch instead.
To adapt easily our changes to this pattern we have to break git best pactices in the following way:
* Every Olimex Branch will be kept as the original
* Every default Olimex branch will have a "*-trexom" branch with Trexom's Cosmo patch applied
This way is also possibile to try to load an original Olimex branch on your Cosmo (voiding Trexom warranty, of course) to check if issues are still present.
Using an original Olimex branch will, of course, make your Cosmo not suitable for selling, since will lack some of the basic features


# Trexom Changes
Trexom applies to Olimex's branches following changes:
* Disabling Temperature Shutdown: SoM's temperature registers lives in RTP's regiter area. Laying your touch panel on an anti-static surface, will make temperature values unreliable, causing random system shutdown, if thermal dts trips are still present.
* Adding Wiegand reader driver
* Realtime audio
* Adding Cosmo Tr3 display support
* Setting Backlight GPIO
* Freeing used GPIOs
* Disabling unused OTG
* Unsetting unused usb power control pins
* Rotating touchscreen coordinates


# New Version Release Policies
Every new Olimex's version release the owner of this repo will:
* Fork locally a new untouched branch. This branch will be an exact mirror of Olimex's branch (ex: release-20220321-v5.10.105).
* Push the freshly created branch (ex: release-20220321-v5.10.105) to this repo.
* Fork locally a new branch, starting from newest olimex's branch (ex: release-20220321-v5.10.105) and naming it appending "-trexom" suffix to the original name (ex: release-20220321-v5.10.105-trexom)
* Push new "*-trexom" branch to this repo.
* Set new "*-trexom" as default
