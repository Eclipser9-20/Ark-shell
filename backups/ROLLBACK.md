# ROLLBACK — restore the known-good terminal look

## Fastest (restore the running shell, no rebuild):
cp ~/ark-terminal/backups/ark.good-chrome.bin /usr/local/bin/ark

## Full source rollback (git):
cd ~/ark-terminal && git checkout ark-good-chrome -- src/chrome.cpp src/chrome.h src/edit.cpp
make && make install

## Or reset the whole tree to the checkpoint commit:
git reset --hard ark-good-chrome   # DESTROYS later work — only if fully wrecked
