; M-l in C-x C-f chooses where files are; C-g there goes back to the
; file name as it was. Choosing Local keeps it too.
; C-x C-f M-l C-g: back; local-location.tmp RET: a new local file
(execute-kbd-macro "\C-x\C-f\M-l\C-glocal-location.tmp\rlocal text\C-x\C-s")
; C-x C-f M-l M-l: back again; M-l Local RET: the same directory; the
; test file RET
(execute-kbd-macro "\C-x\C-f\M-l\M-l\M-lLocal\rlocal-location.input\r")
; C-x i local-location.tmp RET: insert the file saved above
(execute-kbd-macro "\C-xilocal-location.tmp\r")
(shell-command "rm -f local-location.tmp")
(save-buffer)
(save-buffers-kill-emacs)
