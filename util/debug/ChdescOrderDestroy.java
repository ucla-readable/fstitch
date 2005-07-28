import java.io.DataInput;
import java.io.IOException;

class ChdescOrderDestroyFactory extends ModuleOpcodeFactory
{
	public ChdescOrderDestroyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ORDER_DESTROY, "KDB_CHDESC_ORDER_DESTROY");
		addParameter("order", 4);
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
