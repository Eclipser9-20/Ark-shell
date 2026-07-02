# ROLLBACK — restore the known-good terminal look

## Fastest (restore the running shell, no rebuild):
rm -f /usr/local/bin/ark   # fresh inode — avoids stale-cdhash SIGKILL on Apple Silicon
cp ~/ark-terminal/backups/ark.good-chrome.bin /usr/local/bin/ark
codesign --force --sign - /usr/local/bin/ark   # re-sign so AMFI won't kill it
# (an in-place `cp` over an existing Mach-O leaves the kernel's cached cdhash
#  stale -> AMFI SIGKILLs it -> Ghostty "failed to launch" in ~39ms)

## Full source rollback (git):
cd ~/ark-terminal && git checkout ark-good-chrome -- src/chrome.cpp src/chrome.h src/edit.cpp
make && make install

## Or reset the whole tree to the checkpoint commit:
git reset --hard ark-good-chrome   # DESTROYS later work — only if fully wrecked
