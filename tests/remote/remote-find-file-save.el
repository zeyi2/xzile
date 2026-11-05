; Open a file on another machine from C-x C-f, edit and save it.
(shell-command "printf 'remote line\n' > $XZ_REMOTE_HOME/visit.txt")
; C-x C-f M-l xztest RET: choose the machine; visit.txt RET: open the file
; M-> edited remotely C-x C-s: change it and save it there
(execute-kbd-macro "\C-x\C-f\M-lxztest\rvisit.txt\r\M->edited remotely\C-x\C-s")
; C-x b: back to the test buffer, to show the remote file and its backup
(execute-kbd-macro "\C-xbremote-find-file-save.input\r")
(shell-command "cd $XZ_REMOTE_HOME && cat visit.txt && echo backup: && cat visit.txt~" t)
(save-buffer)
(save-buffers-kill-emacs)
