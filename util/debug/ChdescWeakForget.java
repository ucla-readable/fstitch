import java.io.DataInput;
import java.io.IOException;

class ChdescWeakForgetFactory extends ModuleOpcodeFactory
{
	public ChdescWeakForgetFactory(DataInput input)
	{
		super(input, KDB_CHDESC_WEAK_FORGET, "KDB_CHDESC_WEAK_FORGET");
		addParameter("chdesc", 4);
		addParameter("location", 4);
	}
	
	public ChdescWeakForget readChdescWeakForget() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescWeakForget();
	}
}

public class ChdescWeakForget extends Opcode
{
	public ChdescWeakForget(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescWeakForgetFactory(input);
	}
}
