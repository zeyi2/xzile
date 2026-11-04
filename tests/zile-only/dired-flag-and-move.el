; Flag and delete a file, move marked files into a directory, and chmod.
(shell-command "rm -rf dired-tmp2 && mkdir -p dired-tmp2/d && touch dired-tmp2/a dired-tmp2/b dired-tmp2/c dired-tmp2/e")
(dired "dired-tmp2")
; % d ^a$ RET x yes RET: flag a and delete it
; % m ^[bc]$ RET R d RET: move b and c into d
; % m ^e$ RET M 751 RET: change the mode of e
(execute-kbd-macro "%d^a$\rxyes\r%m^[bc]$\rR\C-a\C-kd\r%m^e$\rM751\rq\C-u\M-!find dired-tmp2 | sort; ls -l dired-tmp2/e | cut -c1-10\r\M-!rm -rf dired-tmp2\r\C-x\C-s\C-x\C-c")
