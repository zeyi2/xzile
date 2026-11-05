; Magit on a repository on another machine: git runs there, through
; ssh, in a directory whose name has quotes, spaces and $(x), on files
; whose names have them too. The same directory here, through a
; symbolic link, is another repository, whose git runs here.
(shell-command "cd $XZ_REMOTE_HOME && rm -rf it* moved notrepo link origin.git && mkdir notrepo && git init -q --bare origin.git && printf '[user]\n\tname = Remote\n\temail = r@r\n[commit]\n\tgpgsign = false\n' > .gitconfig && printf 'it\047s a \042repo\042 $(x)' | xargs -0 mkdir && ln -s it* link && cd it* && git init -q -b main && seq 10 > a.txt && echo c > c.txt && git add . && GIT_AUTHOR_DATE='@0 +0000' GIT_COMMITTER_DATE='@0 +0000' git -c user.name=Local -c user.email=l@l -c commit.gpgsign=false commit -qm init && seq 10 | sed -e s/^1$/ONE/ -e s/^10$/TEN/ > a.txt && echo C >> c.txt && printf 'b it\047s \042$(x)\042.txt' | xargs -0 touch && git remote add origin $XZ_REMOTE_HOME/origin.git")
; C-x d M-l xztest RET RET: ~ there; C-s it's a RET RET: into the
; repository; M-x magit-status: its status, there.
(execute-kbd-macro "\C-xd\M-lxztest\r\r\C-sit's a\r\r")
(execute-kbd-macro "\M-xmagit-status\r")
; Stage the first hunk of a.txt (a patch on git's standard input), the
; untracked file, and c.txt, then unstage c.txt.
(execute-kbd-macro "\C-smodified   a.txt\r\tns")
(execute-kbd-macro "\M-<\C-sb it's\rs")
(execute-kbd-macro "\M-<\C-smodified   c.txt\rs\M-<\C-sStaged changes\r\C-smodified   c.txt\ru")
; C-x b RET: back to the status buffer.
(execute-kbd-macro "\C-xh\M-w\C-xbremote-magit.input\r\C-y\C-xb\r")
; From the status buffer, shell-command runs in the repository there:
; a pre-commit hook that takes its time.
(shell-command "printf '#!/bin/sh\nsleep 4\n' > .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit")
; c c: the message, from the commit buffer to git's standard input;
; C-c C-c then C-g cancels the commit, as does remote-timeout; the
; commit buffer stays.
(execute-kbd-macro "cc\C-a\C-kremote change\C-c\C-c\C-g")
(setq remote-timeout 2)
(execute-kbd-macro "\C-c\C-c")
(setq remote-timeout 30)
(shell-command "rm $XZ_REMOTE_HOME/it*/.git/hooks/pre-commit")
(execute-kbd-macro "\C-c\C-c")
; P p: push, in the background, by git there.
(execute-kbd-macro "Pp")
(sit-for 2)
; RET on c.txt: visit it there, at its change.
(execute-kbd-macro "\M-<\C-smodified   c.txt\r\r")
(execute-kbd-macro "\C-xh\M-w\C-xbremote-magit.input\r\C-y")
; A directory given to magit-status is on the current buffer's machine,
; relative to its directory there: from the Dired buffer of ~, link is
; the same repository, in the same buffer; from that buffer, ../notrepo
; is not a repository, and ../missing is not there. The two that fail
; make no buffer.
(switch-to-buffer "home")
(magit-status "link")
(magit-status "../notrepo")
(magit-status "../missing")
; A refresh that cannot run git there keeps the buffer as it was:
; with the repository moved away, g fails; moved back, g works.
(magit-status)
(shell-command "cd && mv it* moved")
(execute-kbd-macro "g\C-xbremote-magit.input\r")
(shell-command "cd $XZ_REMOTE_HOME && printf 'it\047s a \042repo\042 $(x)' | xargs -0 mv moved")
(execute-kbd-macro "\C-xb\rg")
; The same directory here is another repository, in a buffer of its
; own; staging there runs git here, not through ssh.
(switch-to-buffer "remote-magit.input")
(shell-command "rm -f remote-magit-link && ln -s $XZ_REMOTE_HOME/link remote-magit-link && grep -c 'on xztest' $XZILE_DEBUG_LOG > remote-magit.count")
(magit-status "remote-magit-link")
(execute-kbd-macro "\C-smodified   c.txt\rs")
(switch-to-buffer "remote-magit.input")
(shell-command "test $(grep -c 'on xztest' $XZILE_DEBUG_LOG) = $(cat remote-magit.count) && echo staged here, not through ssh; rm -f remote-magit-link remote-magit.count" t)
; Each Magit buffer and c.txt: name, mode and location.
(end-of-buffer)
(list-buffers)
(other-window 1)
(mark-whole-buffer)
(copy-region-as-kill (point) (mark))
(other-window -1)
(yank)
(set-mark (point))
(search-backward "CRM Buffer")
(shell-command-on-region (mark) (point) "grep -e ' magit' -e ' c[.]txt' | sed -e 's/^ [^ ]*  *//' -e 's/  *[0-9][0-9]*  */ | /' -e 's/ *$//' -e 's/   */ | /g'" t t)
; One commit, there (by the remote user), of the first hunk of a.txt
; and the new file; c.txt is staged here.
(end-of-buffer)
(shell-command "cd $XZ_REMOTE_HOME/it* && git log --format='%an: %s' && git show --format= --stat=60 HEAD | sed 's/  *$//' && git show --format= HEAD -- a.txt | grep '^[-+][^-+]' && git status --short && git -C ../origin.git log --format='pushed: %s' main" t)
(save-buffer)
(save-buffers-kill-emacs)
