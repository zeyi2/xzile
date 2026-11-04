; Stage a hunk and commit it from magit-status.
(shell-command "rm -rf magit-tmp1 && mkdir magit-tmp1 && cd magit-tmp1 && git init -q -b main && git config user.email t@t && git config user.name T && printf 'one\ntwo\n' > a.txt && git add a.txt && git commit -qm init && printf 'one\nTWO\nthree\n' > a.txt && echo u > u.txt")
(magit-status "magit-tmp1")
; C-s modified RET TAB: expand a.txt; n s: stage its hunk
; c c first change C-c C-c: commit
(execute-kbd-macro "\C-smodified\r\tnscc\C-a\C-kfirst change\C-c\C-c")
(switch-to-buffer "magit-stage-commit.input")
(shell-command "cd magit-tmp1 && git log --format=%s && git status --short && git show --stat --format= HEAD | tail -1; cd .. && rm -rf magit-tmp1" t)
(save-buffer)
(save-buffers-kill-emacs)
