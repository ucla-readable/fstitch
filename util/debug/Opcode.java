public abstract class Opcode
{
	public static String render(short opcodeNumber)
	{
		String hex = Integer.toHexString(opcodeNumber);
		while(hex.length() < 4)
			hex = "0" + hex;
		return "0x" + hex;
	}
}
