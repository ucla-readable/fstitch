import java.io.DataInput;
import java.io.IOException;

class ChdescCreateNoopFactory extends ModuleOpcodeFactory
{
	public ChdescCreateNoopFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CREATE_NOOP);
		addParameter("chdesc", 4);
		addParameter("block", 4);
		addParameter("owner", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_CREATE_NOOP"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescCreateNoop readChdescCreateNoop() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescCreateNoop();
	}
}

public class ChdescCreateNoop extends Opcode
{
	public ChdescCreateNoop(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescCreateNoopFactory(input);
	}
}
