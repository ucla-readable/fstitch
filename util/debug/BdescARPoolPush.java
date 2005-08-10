import java.io.DataInput;
import java.io.IOException;

public class BdescARPoolPush extends Opcode
{
	private final int depth;
	
	public BdescARPoolPush(int depth)
	{
		this.depth = depth;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_BDESC_AR_POOL_PUSH: depth = " + depth;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_AR_POOL_PUSH, "KDB_BDESC_AR_POOL_PUSH", BdescARPoolPush.class);
		factory.addParameter("depth", 4);
		return factory;
	}
}