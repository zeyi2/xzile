; Create new files on another machine: they exist only once saved. In
; a remote buffer, C-x C-f starts on the same machine.
(shell-command "rm -f $XZ_REMOTE_HOME/new.txt $XZ_REMOTE_HOME/second.txt")
; C-x C-f M-l xztest RET new.txt RET: a new, empty buffer
; C-x C-s: nothing to save yet; then type a line and save it there
(execute-kbd-macro "\C-x\C-f\M-lxztest\rnew.txt\r\C-x\C-sbrand new\C-x\C-s")
; C-x C-f second.txt RET: another new file on xztest, beside new.txt
(execute-kbd-macro "\C-x\C-fsecond.txt\rsecond\C-x\C-s")
(execute-kbd-macro "\C-xbremote-new-file.input\r")
(shell-command "cd $XZ_REMOTE_HOME && cat new.txt second.txt && ls new.txt* second.txt*" t)
(save-buffer)
(save-buffers-kill-emacs)
