; A remote terminal's directory follows OSC 7 only for its machine:
; host names are compared ignoring case, and a short name matches it
; with a domain added, but a longer name with a domain is another host.
; xzfq.example is the test server, under a name with a domain.
(shell-command "rm -rf $XZ_REMOTE_HOME/fq && mkdir -p $XZ_REMOTE_HOME/fq/a $XZ_REMOTE_HOME/fq/b $XZ_REMOTE_HOME/fq/c")
(execute-kbd-macro "\C-xd\M-lxzfq.example\r\r")
(execute-kbd-macro "\M-xshell\r")
(sit-for 1)
(execute-kbd-macro "printf '\\033]7;file://%s%s\\007' xzfq.example.other $HOME/fq/a\r")
(sit-for 1)
(execute-kbd-macro "\M-xshell-command\recho 1:$(pwd | sed s,^$HOME,~,) >> ~/fq/out\r")
(execute-kbd-macro "printf '\\033]7;file://%s%s\\007' XZFQ.Example $HOME/fq/a\r")
(sit-for 1)
(execute-kbd-macro "\M-xshell-command\recho 2:$(pwd | sed s,^$HOME,~,) >> ~/fq/out\r")
(execute-kbd-macro "printf '\\033]7;file://%s%s\\007' xzfq.other $HOME/fq/c\r")
(sit-for 1)
(execute-kbd-macro "\M-xshell-command\recho 3:$(pwd | sed s,^$HOME,~,) >> ~/fq/out\r")
(execute-kbd-macro "printf '\\033]7;file://%s%s\\007' xzfq $HOME/fq/b\r")
(sit-for 1)
(execute-kbd-macro "\M-xshell-command\recho 4:$(pwd | sed s,^$HOME,~,) >> ~/fq/out\r")
(switch-to-buffer "remote-osc-host.input")
(shell-command "cat $XZ_REMOTE_HOME/fq/out" t)
(save-buffer)
(execute-kbd-macro "\C-x\C-cyes\r")
