; With a prefix argument, goto-line and kill-line use it instead of
; prompting or killing to the end of the line, and shell-command inserts
; its output.
; C-u 3 M-x goto-line RET C-u 2 M-x kill-line RET
; C-u M-x shell-command RET echo hi RET save-buffer save-buffers-kill-emacs
(execute-kbd-macro "\C-u3\M-xgoto-line\r\C-u2\M-xkill-line\r\C-u\M-xshell-command\recho hi\r\C-x\C-s\C-x\C-c")
