; Rename, copy, delete marked files, and create a directory in Dired.
(shell-command "rm -rf dired-tmp1 && mkdir dired-tmp1 && touch dired-tmp1/a dired-tmp1/b dired-tmp1/c")
(dired "dired-tmp1")
; R z RET: rename a (the first file) to z; C y RET: copy z to y
; % m ^[bc]$ RET D yes RET: delete b and c; + sub RET: make a directory
; q: back to the test buffer; C-u M-! ls: insert the result
(execute-kbd-macro "R\C-a\C-kz\rC\C-a\C-ky\r%m^[bc]$\rDyes\r+\C-a\C-ksub\rq\C-u\M-!ls dired-tmp1\r\M-!rm -rf dired-tmp1\r\C-x\C-s\C-x\C-c")
