; Browse a remote directory with C-x d: open files and directories, go
; up, and refresh. Commands that change files are refused there.
(shell-command "rm -rf $XZ_REMOTE_HOME/proj && mkdir -p $XZ_REMOTE_HOME/proj/sub && printf 'alpha\n' > $XZ_REMOTE_HOME/proj/a.txt && printf 'beta\n' > $XZ_REMOTE_HOME/proj/b.txt && touch $XZ_REMOTE_HOME/proj/sub/inner")
; C-x d M-l xztest RET proj RET: list ~/proj on xztest, at a.txt
; n n RET: into sub; ^: back up, at sub; g: reread; w: copy its name
(execute-kbd-macro "\C-xd\M-lxztest\rproj\rnn\r^gw")
(execute-kbd-macro "\C-xbremote-dired.input\r\C-y\r\C-xbproj\r")
; R C D x M +: all refused, so nothing changes
(execute-kbd-macro "RCDxM+")
; p p RET: open a.txt and copy its text; C-x C-j: back to its
; directory, at a.txt, and copy that name
(execute-kbd-macro "pp\r\C-xh\M-w\C-xbremote-dired.input\r\C-y\C-xba.txt\r\C-x\C-jw")
(execute-kbd-macro "\C-xbremote-dired.input\r\C-y\r")
(shell-command "cd $XZ_REMOTE_HOME/proj && ls -A . sub" t)
(save-buffer)
(save-buffers-kill-emacs)
