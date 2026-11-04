; Saving over a file changed on disk writes the buffer once confirmed.
(insert "buffer edit ")
(shell-command "echo disk version > save-changed-on-disk-yes.input")
(execute-kbd-macro "\C-x\C-syes\r\C-x\C-c")
