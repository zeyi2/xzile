; In a remote buffer, shell-command and shell-command-on-region run on
; its machine, in its directory there, with the region as input; in a
; local buffer they still run here. The directory's name has quotes,
; spaces, $(x) and a backslash, which must reach cd as they are.
(shell-command "rm -rf $XZ_REMOTE_HOME/q* $XZ_REMOTE_HOME/moved $XZ_REMOTE_HOME/late && cd $XZ_REMOTE_HOME && printf 'q it\047s \042$(x)\042 \134d' | xargs -0 mkdir && cd q* && printf 'b\na\nc\n' > f.txt")
(shell-command "echo local: ${XZ_REMOTE_HOME:+has XZ_REMOTE_HOME} ${SSH_CONNECTION:-no ssh}" t)
(forward-line)
; C-x d M-l xztest RET RET: Dired of ~ there; C-s q it RET RET: into q...
(execute-kbd-macro "\C-xd\M-lxztest\r\r")
(execute-kbd-macro "\C-sq it\r")
(execute-kbd-macro "\r")
; From Dired: the remote directory and environment, standard error,
; and a command with quotes kept as they are; no input.
(shell-command "pwd | sed s,^$HOME,~, ; echo remote: ${XZ_REMOTE_HOME:-no XZ_REMOTE_HOME} ${SSH_CONNECTION:+over ssh}; echo to-stderr >&2; printf '%s|' it\'s '$HOME' 'a\b'; echo; cat; echo stdin closed")
; Open f.txt: C-u M-| sort replaces the region with remote output.
(execute-kbd-macro "\C-sf.txt\r")
(execute-kbd-macro "\r\C-xh\C-u\M-|sort\r")
; C-u M-! inserts at point; an exit status of 255 is the command's own.
(execute-kbd-macro "\M->\C-u\M-!printf 'two five five'; exit 255\r")
; The exact bytes of the region, without a final newline, go to stdin.
(execute-kbd-macro "\M->\C-xh\C-u\M-|tee ~/region.copy; echo; wc -c < ~/region.copy\r")
; Failures change nothing: a command killed before it ends...
(execute-kbd-macro "\C-xh\C-u\M-|printf replaced; [ ${SSH_CONNECTION:+x}x = xx ] && kill -9 $PPID\r")
; ... cancelled with C-g ...
(execute-kbd-macro "\C-xh\C-u\M-|sleep 5; printf replaced\r\C-g")
; ... timed out (and stopped there) ...
(setq remote-timeout 2)
(execute-kbd-macro "\C-xh\C-u\M-|sleep 4; touch ~/late; printf replaced\r")
(setq remote-timeout 30)
; ... or with its directory gone.
(execute-kbd-macro "\C-xbremote-shell-command.input\r")
(shell-command "mv $XZ_REMOTE_HOME/q* $XZ_REMOTE_HOME/moved")
(execute-kbd-macro "\C-xbf.txt\r\C-xh\C-u\M-|printf replaced\r")
(execute-kbd-macro "\C-xb*Shell Command Output*\r\C-xh\M-w\C-xbremote-shell-command.input\r\C-y")
(execute-kbd-macro "\C-xbf.txt\r\C-xh\M-w\C-xbremote-shell-command.input\r\C-y")
(sit-for 3)
(shell-command "test -e $XZ_REMOTE_HOME/late && echo the timed-out command went on || echo the timed-out command was stopped; cat $XZ_REMOTE_HOME/moved/f.txt" t)
(save-buffer)
; f.txt was edited, but is not saved.
(execute-kbd-macro "\C-x\C-cnyes\r")
