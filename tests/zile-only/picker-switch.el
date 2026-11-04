; C-x b narrows fuzzily: "alpbuf" selects alpha-buffer. RET with empty
; input returns to the previous buffer.
(switch-to-buffer "alpha-buffer")
(insert "alpha text")
(switch-to-buffer "picker-switch.input")
(execute-kbd-macro "\C-xbalpbuf\r\C-xh\M-w\C-xb\r\C-y\r")
(save-buffer)
(save-buffers-kill-emacs)
