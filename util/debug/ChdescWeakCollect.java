import java.io.DataInput;
import java.io.IOException;

class ChdescWeakCollectFactory extends ModuleOpcodeFactory
{
	public ChdescWeakCollectFactory(DataInput input)
	{
		super(input, KDB_CHDESC_WEAK_COLLECT, "KDB_CHDESC_WEAK_COLLECT");
		addParameter("chdesc", 4);
	}
	
	public ChdescWeakCollect readChdescWeakCollect() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescWeakCollect();
	}
}

public class ChdescWeakCollect extends Opcode
{
	public ChdescWeakCollect(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescWeakCollectFactory(input);
	}
}
