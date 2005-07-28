import java.io.DataInput;
import java.io.IOException;

class ChdescDestroyFactory extends ModuleOpcodeFactory
{
	public ChdescDestroyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_DESTROY, "KDB_CHDESC_DESTROY");
		addParameter("chdesc", 4);
	}
	
	public ChdescDestroy readChdescDestroy() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescDestroy();
	}
}

public class ChdescDestroy extends Opcode
{
	public ChdescDestroy(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescDestroyFactory(input);
	}
}
