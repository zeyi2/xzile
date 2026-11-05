; C-u 2 M-x forward-char RET a M-3 M-x forward-char RET b
; C-u M-x forward-char RET c save-buffer save-buffers-kill-emacs
(execute-kbd-macro "\C-u2\M-xforward-char\ra\M-3\M-xforward-char\rb\C-u\M-xforward-char\rc\C-x\C-s\C-x\C-c")
