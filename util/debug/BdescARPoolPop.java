import java.io.DataInput;
import java.io.IOException;

public class BdescARPoolPop extends Opcode
{
	public BdescARPoolPop(int depth)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_AR_POOL_POP, "KDB_BDESC_AR_POOL_POP", BdescARPoolPop.class);
		factory.addParameter("depth", 4);
		return factory;
	}
}
