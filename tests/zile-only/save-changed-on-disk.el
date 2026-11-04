; Saving over a file changed on disk asks first; "no" keeps the disk
; version, which revert-buffer then loads.
(insert "buffer edit ")
(shell-command "echo disk version > save-changed-on-disk.input")
(execute-kbd-macro "\C-x\C-sno\r\M-xrevert-buffer\ryes\r\C-x\C-c")
