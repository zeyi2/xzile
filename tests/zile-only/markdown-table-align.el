; markdown-table-align pads the cells of the table point is in to the
; widths of their columns, as its delimiter row aligns them, with CJK
; characters two columns wide; pipes escaped or in code spans are text.
; Rows get the header's cells, and keep those beyond. Point stays in
; its cell (@). A line with a pipe but no delimiter row after it, and a
; table in fenced code, are not tables (%, #).
(markdown-mode)
(setq case-fold-search nil)
(markdown-table-align)
(insert "%")
(search-forward "数量")
(markdown-table-align)
(insert "@")
(search-forward "long")
(markdown-table-align)
(insert "@")
(search-forward "| inner")
(markdown-table-align)
(insert "#")
(save-buffer)
(save-buffers-kill-emacs)
