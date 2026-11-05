; In a remote buffer, commands that would run programs or use local
; files instead are refused; the remote file is left alone.
(shell-command "printf 'keep me\n' > $XZ_REMOTE_HOME/refuse.txt")
(execute-kbd-macro "\C-x\C-f\M-lxztest\rrefuse.txt\r")
(execute-kbd-macro "\M-xshell-command\r\M-xshell-command-on-region\r\M-xshell\r\M-xterm\r")
(execute-kbd-macro "\M-xcompile\r\M-xrecompile\r\M-xmagit-status\r\M-xcd\r\C-x\C-w")
(execute-kbd-macro "\M-ximage-mode\r\M-xpreview-toggle-display\r\M-xhtml-preview\r")
(execute-kbd-macro "\C-x\C-s\C-xbremote-refuse.input\r")
(shell-command "cat $XZ_REMOTE_HOME/refuse.txt" t)
(save-buffer)
(save-buffers-kill-emacs)
