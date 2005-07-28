import java.io.DataInput;
import java.io.IOException;

class BdescFreeDdescFactory extends ModuleOpcodeFactory
{
	public BdescFreeDdescFactory(DataInput input)
	{
		super(input, KDB_BDESC_FREE_DDESC, "KDB_BDESC_FREE_DDESC");
		addParameter("block", 4);
		addParameter("ddesc", 4);
	}
	
	public BdescFreeDdesc readBdescFreeDdesc() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescFreeDdesc();
	}
}

public class BdescFreeDdesc extends Opcode
{
	public BdescFreeDdesc(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescFreeDdescFactory(input);
	}
}
