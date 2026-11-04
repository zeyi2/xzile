; Copy file names to the kill ring and kill their lines in Dired; the
; files themselves remain.
(shell-command "rm -rf dired-tmp3 && mkdir dired-tmp3 && touch dired-tmp3/a dired-tmp3/b dired-tmp3/c")
(dired "dired-tmp3")
; % m ^[ab]$ RET w k: copy the names of a and b, and kill their lines
; q C-y RET: yank the names into the test buffer
(execute-kbd-macro "%m^[ab]$\rwkq\C-y\r\C-u\M-!ls dired-tmp3\r\M-!rm -rf dired-tmp3\r\C-x\C-s\C-x\C-c")
