import java.io.DataInput;
import java.io.IOException;

class ChdescOrderDestroyFactory extends ModuleOpcodeFactory
{
	public ChdescOrderDestroyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ORDER_DESTROY);
		addParameter("order", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_ORDER_DESTROY"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescOrderDestroy readChdescOrderDestroy() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescOrderDestroy();
	}
}

public class ChdescOrderDestroy extends Opcode
{
	public ChdescOrderDestroy(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescOrderDestroyFactory(input);
	}
}
