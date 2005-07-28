import java.io.DataInput;
import java.io.IOException;

class ChdescDuplicateFactory extends ModuleOpcodeFactory
{
	public ChdescDuplicateFactory(DataInput input)
	{
		super(input, KDB_CHDESC_DUPLICATE);
		addParameter("original", 4);
		addParameter("count", 4);
		addParameter("blocks", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_DUPLICATE"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescDuplicate readChdescDuplicate() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescDuplicate();
	}
}

public class ChdescDuplicate extends Opcode
{
	public ChdescDuplicate(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescDuplicateFactory(input);
	}
}
