import java.io.DataInput;
import java.io.IOException;

class BdescARPoolPopFactory extends ModuleOpcodeFactory
{
	public BdescARPoolPopFactory(DataInput input)
	{
		super(input, KDB_BDESC_AR_POOL_POP, "KDB_BDESC_AR_POOL_POP");
		addParameter("depth", 4);
	}
	
	public BdescARPoolPop readBdescARPoolPop() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescARPoolPop();
	}
}

public class BdescARPoolPop extends Opcode
{
	public BdescARPoolPop(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescARPoolPopFactory(input);
	}
}
