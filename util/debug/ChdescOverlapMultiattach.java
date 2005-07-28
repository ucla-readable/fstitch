import java.io.DataInput;
import java.io.IOException;

class ChdescOverlapMultiattachFactory extends ModuleOpcodeFactory
{
	public ChdescOverlapMultiattachFactory(DataInput input)
	{
		super(input, KDB_CHDESC_OVERLAP_MULTIATTACH, "KDB_CHDESC_OVERLAP_MULTIATTACH");
		addParameter("chdesc", 4);
		addParameter("block", 4);
		addParameter("slip_under", 1);
	}
	
	public ChdescOverlapMultiattach readChdescOverlapMultiattach() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescOverlapMultiattach();
	}
}

public class ChdescOverlapMultiattach extends Opcode
{
	public ChdescOverlapMultiattach(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescOverlapMultiattachFactory(input);
	}
}
