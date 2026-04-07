/* C mode */

public class CMode : CFamilyMode {
	private static string[] keywords = {
		"alignas", "alignof", "auto", "bool", "break", "case", "char",
		"const", "constexpr", "continue", "default", "do", "double", "else",
		"enum", "extern", "false", "float", "for", "goto", "if", "inline",
		"int", "long", "nullptr", "register", "restrict", "return", "short",
		"signed", "sizeof", "static", "static_assert", "struct", "switch",
		"thread_local", "true", "typedef", "typeof", "typeof_unqual", "union",
		"unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
		"_Atomic", "_Bool", "_Complex", "_Decimal32", "_Decimal64",
		"_Decimal128", "_Generic", "_Imaginary", "_Noreturn",
		"_Static_assert", "_Thread_local"
	};

	public CMode () {
		base ("C", keywords);
	}
}
