import java.io.DataInput;
import java.io.IOException;

class BdescARPoolPushFactory extends ModuleOpcodeFactory
{
	public BdescARPoolPushFactory(DataInput input)
	{
		super(input, KDB_BDESC_AR_POOL_PUSH);
		addParameter("depth", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_BDESC_AR_POOL_PUSH"))
			throw new UnexpectedNameException(name);
	}
	
	public BdescARPoolPush readBdescARPoolPush() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescARPoolPush();
	}
}

public class BdescARPoolPush extends Opcode
{
	public BdescARPoolPush(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescARPoolPushFactory(input);
	}
}
