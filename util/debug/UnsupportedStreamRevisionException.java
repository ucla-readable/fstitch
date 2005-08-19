/**
 * An exception thrown when the debug stream's revision is unsupported by
 * this version of kdb.
 * */

public class UnsupportedStreamRevisionException extends BadInputException
{
	private int debugRev;
	private int debugOpcodeRev;
	private int revToUse;

	public UnsupportedStreamRevisionException(int debugRev,
	                                          int debugOpcodeRev,
	                                          int revToUse)
	{
		super("Debug stream revision (" + debugRev + "," + debugOpcodeRev +
		      ") unsupported, use kdb " + revToUse + " or earlier.");
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
