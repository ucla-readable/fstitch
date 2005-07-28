import java.io.DataInput;
import java.io.IOException;

class ChdescOverlapAttachFactory extends ModuleOpcodeFactory
{
	public ChdescOverlapAttachFactory(DataInput input)
	{
		super(input, KDB_CHDESC_OVERLAP_ATTACH, "KDB_CHDESC_OVERLAP_ATTACH");
		addParameter("recent", 4);
		addParameter("original", 4);
	}
	
	public ChdescOverlapAttach readChdescOverlapAttach() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescOverlapAttach();
	}
}

public class ChdescOverlapAttach extends Opcode
{
	public ChdescOverlapAttach(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescOverlapAttachFactory(input);
	}
}
