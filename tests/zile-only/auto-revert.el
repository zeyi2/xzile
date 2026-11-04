; An unmodified buffer follows its file when it changes on disk.
(shell-command "echo old text > auto-revert.tmp")
(find-file "auto-revert.tmp")
(shell-command "echo new text from disk > auto-revert.tmp")
(switch-to-buffer "auto-revert.input")
(execute-kbd-macro "\C-xbauto-revert.tmp\r\C-xh\M-w\C-xb\r\M-<\C-y\C-x\C-s\M-x shell-command\rrm -f auto-revert.tmp\r\C-x\C-c")
