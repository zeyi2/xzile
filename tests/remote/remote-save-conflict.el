; Saving over a remote file changed since it was read asks first;
; "no" keeps the remote file and the unsaved buffer. revert-buffer
; reads the new version, which can then be saved without a question.
(shell-command "printf 'original\n' > $XZ_REMOTE_HOME/conflict.txt")
(execute-kbd-macro "\C-x\C-f\M-lxztest\rconflict.txt\r")
(execute-kbd-macro "\C-xbremote-save-conflict.input\r")
(shell-command "printf 'changed elsewhere\n' > $XZ_REMOTE_HOME/conflict.txt")
; In conflict.txt: edit, C-x C-s no RET: not saved
(execute-kbd-macro "\C-xbconflict.txt\r\M->buffer edit\C-x\C-sno\r")
(execute-kbd-macro "\C-xbremote-save-conflict.input\r")
(shell-command "cat $XZ_REMOTE_HOME/conflict.txt" t)
; Save again, yes RET: written over the other change
(execute-kbd-macro "\C-xbconflict.txt\r\C-x\C-syes\r")
(execute-kbd-macro "\C-xbremote-save-conflict.input\r")
(shell-command "cat $XZ_REMOTE_HOME/conflict.txt" t)
; Change it remotely again, revert, edit and save: no question
(shell-command "printf 'third version\n' > $XZ_REMOTE_HOME/conflict.txt")
(execute-kbd-macro "\C-xbconflict.txt\r\M-xrevert-buffer\ryes\r\M-<reverted \C-x\C-s")
(execute-kbd-macro "\C-xbremote-save-conflict.input\r")
(shell-command "cat $XZ_REMOTE_HOME/conflict.txt" t)
(save-buffer)
(save-buffers-kill-emacs)
