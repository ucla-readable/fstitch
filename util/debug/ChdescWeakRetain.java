import java.io.DataInput;
import java.io.IOException;

class ChdescWeakRetainFactory extends ModuleOpcodeFactory
{
	public ChdescWeakRetainFactory(DataInput input)
	{
		super(input, KDB_CHDESC_WEAK_RETAIN, "KDB_CHDESC_WEAK_RETAIN");
		addParameter("chdesc", 4);
		addParameter("location", 4);
	}
	
	public ChdescWeakRetain readChdescWeakRetain() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescWeakRetain();
	}
}

public class ChdescWeakRetain extends Opcode
{
	public ChdescWeakRetain(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescWeakRetainFactory(input);
	}
}
