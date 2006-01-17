/**
 * This exception is thrown when the input file's revision
 * number is unsupported by this version of the debugger.
 **/

public class UnsupportedStreamRevisionException extends BadInputException
{
	private final int debugRev;
	private final int debugOpcodeRev;
	private final int revToUse;
	
	public UnsupportedStreamRevisionException(int debugRev, int debugOpcodeRev, int revToUse)
	{
		super("Input file revision (" + debugRev + ", " + debugOpcodeRev + ") unsupported; use " + ((revToUse == 0) ? "a newer debugger version." : "debugger version " + revToUse + " or earlier."));
		this.debugRev = debugRev;
		this.debugOpcodeRev = debugOpcodeRev;
		this.revToUse = revToUse;
	}
	
	public int getDebugRev()
	{
		return debugRev;
	}
	
	public int getDebugOpcodeRev()
	{
		return debugOpcodeRev;
	}
	
	public int getRevToUse()
	{
		return revToUse;
	}
}
