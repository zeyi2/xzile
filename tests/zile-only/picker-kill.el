; C-k in C-x b kills the selected buffer; C-j uses the input as typed,
; here creating an empty buffer of the same name.
(switch-to-buffer "doomed")
(insert "doomed text")
(switch-to-buffer "picker-kill.input")
(execute-kbd-macro "\C-xbdoomed\C-k\C-g\C-xbdoomed\C-j\C-xh\M-w\C-xb\r[\C-y]\r")
(save-buffer)
(save-buffers-kill-emacs)
